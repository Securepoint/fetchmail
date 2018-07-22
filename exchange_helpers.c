#include "sp_fetchmail.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>

#if 0
/* https://technet.microsoft.com/en-us/library/bb331962(v=exchg.141).aspx
 *first mimepart fields:
 *
 * Basic fields:
 *  Sender
 *  Subject
 *  Message-ID
 *  Recipient
 *
 * Extended fields:
 *  On-Behalf-Of
 *  To
 *  Cc
 *  Bcc
 *
 * Expanded and Forwarded Fields:
 *  Expanded
 *  Forwarded
 *  
 *
 * Examples:
 * To: garrett.gelb@spqa2.local, Expanded: Warmfarben@spqa2.local 
 * To: xaviera.xanthic@spqa2.local, Expanded: Dunkelfarben@spqa2.local
 * Cc: arina@spqa2.local
 * Cc: quelaag.quittengelb@spqa2.local
 * Bcc: doerte.dunkelbraun@spqa2.local
 * Sender: caul.cyal@spqa2.local
 * Subject: Test im auftrag von cyan
 * Message-Id: <20160720111023.00005d68@spqa2.local>
 * Recipient: joel.jadegruen@spqa2.local
 *
 *
 * */
#endif
void parse_ms_envelope(char *content, size_t content_len,
  addressinfo_t * addinfo)
{

  char *endl, *startl, *tmpc;
  int left, n;
  int sender = 0;

  left = content_len;
  startl = content;

  /* iterate over the block */
  while ((1 < left) && (endl = memchr(startl, '\n', left))) {
    if ('\r' == *(endl - 1))
      memset(endl - 1, 0, 2);
    else
      memset(endl, 0, 1);
    left -= (endl - startl) + 1;

    if ((n = 3) && !strncasecmp(startl, "to:", n)) {
    } else if ((n = 10) && !strncasecmp(startl, "recipient:", n)) {
    } else if ((n = 3) && !strncasecmp(startl, "cc:", n)) {
    } else if ((n = 4) && !strncasecmp(startl, "bcc:", n)) {
    } else if ((n = 7) && !strncasecmp(startl, "sender:", n)) {
      sender = 1;
    } else {
      goto cont;
    }
    startl += n;

    /* check if there is an extension attribute to the field */
    if (memchr(startl, ',', (endl - startl))) {
      if ((tmpc = strcasestr(startl, "expanded:"))) {
        startl = tmpc + 9;
      } else if ((tmpc = strcasestr(startl, "forwarded:"))) {
        /* ignore forwarded lines */
        goto cont;
      } else {
        debug("unknown extension field %s", startl);
      }
    }
    /* remove leading spaces */
    while (' ' == *startl)
      startl++;

    if (!addinfo->sender && sender) {
      addinfo->sender = strdup(startl);
    } else {
      clist_uniadd(addinfo->recipients, startl);
    }

  cont:
    startl = endl + 1;
  }
}

int get_msrecipients(struct mailmessage *msg, addressinfo_t * addinfo)
{

  char *rawcontent = NULL;
  size_t rawlen;
  struct mailmime *mime = NULL;
  int tmpret, ret = 1;

  if ((tmpret = mailmessage_get_bodystructure(msg, &msg->msg_mime))) {
    debug("cant parse structure of mail id %d (%d)", msg->msg_index, tmpret);
  } else if (!msg->msg_mime->mm_data.mm_message.mm_msg_mime
    || !msg->msg_mime->mm_data.mm_message.mm_msg_mime->mm_data.mm_multipart.
    mm_mp_list) {
    debug("mime structure doesnt look like MS-Journal");
  } else if (2 >
    clist_count(msg->msg_mime->mm_data.mm_message.mm_msg_mime->mm_data.
      mm_multipart.mm_mp_list)) {
    /* blurp */
  } else if (!(mime =
      clist_nth_data(msg->msg_mime->mm_data.mm_message.mm_msg_mime->mm_data.
        mm_multipart.mm_mp_list, 0))) {
    /* hurz */
  } else if ((tmpret =
      mailmessage_fetch_section(msg, mime, &rawcontent, &rawlen))) {
    debug("cant fetch envelope mime content of mail id %d (%d)",
      msg->msg_index, tmpret);
  } else {
    parse_ms_envelope(rawcontent, rawlen, addinfo);
    if (0 < clist_count(addinfo->recipients))
      ret = 0;
  }

  if (rawcontent)
    mailmessage_fetch_result_free(msg, rawcontent);

  return ret;
}

int check_for_msenvelope(struct mailmessage *msg)
{

  char *rawcontent = NULL;
  struct mailimf_fields *fields = NULL;
  int ret = 1;

  if (!msg)
    return -1;

  if ((init_msg_headerfields(msg))) {
    debug("error on headerparsing");
  } else if (!get_optional_field_p(msg->msg_fields, "X-MS-Journal-Report")) {
    /* keine arme, keine kekse */
  } else {
    ret = 0;
  }

  if (fields)
    mailimf_fields_free(fields);
  if (rawcontent)
    mailmessage_fetch_result_free(msg, rawcontent);

  return ret;
}

/*
 *
 *
 *
 *
 */
int check_for_msjournal(struct mailmessage *msg)
{

  addressinfo_t *addinfo = NULL;
  int ret = 1;
  struct mailmime *mime = NULL;
  char *data = NULL;
  size_t len;

  if (clist_inlistremove(evilmails, msg->msg_uid)) {
    debug("mail had problems with parsing. will do non envelope fetch");
  } else if (check_for_msenvelope(msg)) {
    /* no msmail */
  } else if (!(addinfo = addinfo_new())) {
    debug("oom in %s", __func__);
  } else if (get_msrecipients(msg, addinfo)) {
    debug("found journalheader but no envelope recipients in mail id %d",
      msg->msg_index);
    clist_uniadd(evilmails, msg->msg_uid);
  } else if (!(mime =
      clist_nth_data(msg->msg_mime->mm_data.mm_message.mm_msg_mime->mm_data.
        mm_multipart.mm_mp_list, 1))) {
    debug("no mail-mimepart found in mail id %d", msg->msg_index);
  } else if ((ret = mailmessage_fetch_section(msg, mime, &data, &len))) {
    debug("cant fetch mime content of mail id %d (%d)", msg->msg_index, ret);
    //} else if (!(headerend = strstr(data, "\r\n\r\n"))) {
    //  debug("cant detect header in mime part mail id %d", msg->msg_index);
  } else {
    ret = deliver_lmtp(data, len, addinfo, NULL);
  }

  if (data)
    mailmessage_fetch_result_free(msg, data);

/*  } else {
    //fprintf(stderr, "sender is: %s\nrecipients:\n", addinfo->sender);
    //for (clistiter *iter = clist_begin(addinfo->recipients); iter;
    //  iter = clist_next(iter)) {
    //  fprintf(stderr, " %s\n", (char *) clist_content(iter));
    //}
    ret = deliver_msjournal_mail(addinfo, msg);
  }*/

  if (addinfo)
    addinfo_free(addinfo);

  return ret;
}

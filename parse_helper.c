#include "sp_fetchmail.h"
#include <stdlib.h>
#include <string.h>

#if 0
old beshaviour
  RECIPIENT ADDRESSES
  qmail - inject looks for recipient address lists in the following
  fields:To, Cc, Bcc, Apparently - To, Resent - To, Resent - Cc,
  Resent - Bcc.SENDER ADDRESSES qmail - inject looks for sender address lists
  in the following fields:Sender, From,
  Reply - To, Return - Path, Return - Receipt - To, Errors - To,
  Resent - Sender, Resent - From, Resent - Reply - To.
#endif
int get_stdheader_info(mailmessage * msg, addressinfo_t ** addi)
{

  clistiter *iter, *iterg, *outeriter;
  struct mailimf_field *field;
  struct mailimf_address *addr;
  struct mailimf_mailbox *mb;
  struct mailimf_address_list *addls;
  int t;
  int resent = 0;               // RESENT-XXXX Headers present? (overwrites non RESENT Headers)
  const char *sender = NULL;    // Es kann nur einen geben!
  int senderprio = 99;          // 0 highest(sender-field); 1==from-field, ......

  if (!msg || !(msg->msg_fields))
    return -1;

  for (outeriter = clist_begin(msg->msg_fields->fld_list); outeriter;
    outeriter = clist_next(outeriter)) {
    addls = NULL;
    field = clist_content(outeriter);
    t = field->fld_type;

    if (!resent && MAILIMF_FIELD_TO == t) {
      if (field->fld_data.fld_to
        && field->fld_data.fld_to->to_addr_list->ad_list) {
        addls = field->fld_data.fld_to->to_addr_list;
      }
    } else if (!resent && MAILIMF_FIELD_CC == t) {
      if (field->fld_data.fld_cc
        && field->fld_data.fld_cc->cc_addr_list->ad_list) {
        addls = field->fld_data.fld_cc->cc_addr_list;
      }
    } else if (!resent && MAILIMF_FIELD_BCC == t) {
      if (field->fld_data.fld_bcc
        && field->fld_data.fld_bcc->bcc_addr_list->ad_list) {
        addls = field->fld_data.fld_bcc->bcc_addr_list;
      }
    } else if (MAILIMF_FIELD_RESENT_TO == t) {
      if (field->fld_data.fld_resent_to
        && field->fld_data.fld_resent_to->to_addr_list->ad_list) {
        addls = field->fld_data.fld_resent_to->to_addr_list;
        resent++;
      }
    } else if (MAILIMF_FIELD_RESENT_CC == t) {
      if (field->fld_data.fld_resent_cc
        && field->fld_data.fld_resent_cc->cc_addr_list->ad_list) {
        addls = field->fld_data.fld_resent_cc->cc_addr_list;
        resent++;
      }
    } else if (MAILIMF_FIELD_RESENT_BCC == t) {
      if (field->fld_data.fld_bcc
        && field->fld_data.fld_resent_bcc->bcc_addr_list->ad_list) {
        addls = field->fld_data.fld_resent_bcc->bcc_addr_list;
        resent++;
      }
    }
    // check if we found recipients and add them
    if (addls) {
      if (1 == resent) {
        /* clear recip list from non RESENT- headers */
        resent++;
        clist_free_withcontent((*addi)->recipients);
        (*addi)->recipients = clist_new();
      }

      for (iter = clist_begin(addls->ad_list); iter; iter = clist_next(iter)) {
        addr = clist_content(iter);
        if ((MAILIMF_ADDRESS_MAILBOX == addr->ad_type)) {
          if ((addr->ad_data.ad_mailbox)
            && (addr->ad_data.ad_mailbox->mb_addr_spec)) {
            clist_uniadd((*addi)->recipients,
              addr->ad_data.ad_mailbox->mb_addr_spec);
            //printf("recipient found is %s\n", addr->ad_data.ad_mailbox->mb_addr_spec);
          } else if (MAILIMF_ADDRESS_GROUP == addr->ad_type) {
            if ((addr->ad_data.ad_group)
              && (addr->ad_data.ad_group->grp_mb_list)) {
              for (iterg =
                clist_begin(addr->ad_data.ad_group->grp_mb_list->mb_list);
                iterg; iterg = clist_next(iterg)) {
                mb = clist_content(iterg);
                if (mb->mb_addr_spec) {
                  clist_uniadd((*addi)->recipients, mb->mb_addr_spec);
                  //printf("recipient found is %s\n", mb->mb_addr_spec);
                }
              }
            }
          }
        }
      }

      /* sender fields */
    } else if (MAILIMF_FIELD_RESENT_SENDER == t) {
      if (field->fld_data.fld_resent_sender->snd_mb
        && field->fld_data.fld_resent_sender->snd_mb->mb_addr_spec) {
        sender = field->fld_data.fld_resent_sender->snd_mb->mb_addr_spec;
        //printf("SENDER is %s: \n", sender);
        senderprio = 0;
      }
    } else if (MAILIMF_FIELD_RESENT_FROM == t) {
      if (senderprio > 0) {
        if (field->fld_data.fld_resent_from
          && field->fld_data.fld_resent_from->frm_mb_list->mb_list
          && !clist_isempty(field->fld_data.fld_resent_from->
            frm_mb_list->mb_list)) {
          iter =
            clist_end(field->fld_data.fld_resent_from->frm_mb_list->mb_list);
          mb = clist_content(iter);
          sender = mb->mb_addr_spec;
          //printf("FROM is %s: \n", sender);
          senderprio = 1;
        }
      }
    } else if (MAILIMF_FIELD_SENDER == t) {
      if (senderprio > 1) {
        if (field->fld_data.fld_sender->snd_mb
          && field->fld_data.fld_sender->snd_mb->mb_addr_spec) {
          sender = field->fld_data.fld_sender->snd_mb->mb_addr_spec;
          //printf("SENDER is %s: \n", sender);
          senderprio = 2;
        }
      }
    } else if (MAILIMF_FIELD_FROM == t) {
      if (senderprio > 2) {
        if (field->fld_data.fld_from
          && field->fld_data.fld_from->frm_mb_list->mb_list
          && !clist_isempty(field->fld_data.fld_from->frm_mb_list->mb_list)) {
          iter = clist_end(field->fld_data.fld_from->frm_mb_list->mb_list);
          mb = clist_content(iter);
          sender = mb->mb_addr_spec;
          //printf("FROM is %s: \n", sender);
          senderprio = 3;
        }
      }
    } else if (MAILIMF_FIELD_REPLY_TO == t) {
      if (senderprio > 3) {
        if (field->fld_data.fld_reply_to
          && field->fld_data.fld_reply_to->rt_addr_list
          && field->fld_data.fld_reply_to->rt_addr_list->ad_list) {
          for (iter =
            clist_begin(field->fld_data.fld_reply_to->rt_addr_list->ad_list);
            iter; iter = clist_next(iter)) {
            addr = clist_content(iter);
            if ((addr->ad_data.ad_mailbox)
              && (addr->ad_data.ad_mailbox->mb_addr_spec)) {
              //printf("reply-to is %s | ",
              //  addr->ad_data.ad_mailbox->mb_addr_spec);
              sender = addr->ad_data.ad_mailbox->mb_addr_spec;
              senderprio = 4;
            }
          }
        }
      }
    }
  }

  if (sender) {
    //printf("found Sender is %s: \n", sender);
    (*addi)->sender = strdup(sender);
  }
  return 0;
}

int get_userheader_recips(mailmessage * msg, const char *optfield,
  addressinfo_t ** addi)
{
  char *value;
  int ret = 1;

  if (!(value = get_optional_field(msg->msg_fields, optfield))) {
    debug("header %s not found", optfield);
    //} else if (0 < isin_archive_domains(value)){
  } else {
    clist_uniadd((*addi)->recipients, value);
    ret = 0;
  }
  return ret;
}

/* resolves header infos in struct mailmessage */
int init_msg_headerfields(mailmessage * msg)
{

  char *rawcontent = NULL;
  size_t rawlen, indx = 0;
  int ret = 1;

  if (!(msg->msg_fields)) {
    if ((ret = mailmessage_fetch_header(msg, &rawcontent, &rawlen))) {
      debug("cant fetch header of mail id %d (%d)", msg->msg_index, ret);
      /* put on evilmail list only on fetch errors, not if session is screwed */
      if (ret != MAIL_ERROR_PROTOCOL)
        clist_uniadd(evilmails, msg->msg_uid);
    } else if ((ret =
        mailimf_envelope_and_optional_fields_parse(rawcontent, rawlen, &indx,
          &msg->msg_fields))) {
      debug("cant parse header of mail id %d (%d)", msg->msg_index, ret);
    }
  } else {
    /* already fetched */
    ret = 0;
  }

  if (rawcontent)
    mailmessage_fetch_result_free(msg, rawcontent);
  return ret;
}

/* 
 */

int check_header(mailmessage * msg, addressinfo_t * addi)
{

  int ret = 1;
  char *data = NULL;
  size_t datalen;

  if (!addi)
    addi = addinfo_new();

  if (init_msg_headerfields(msg)) {
    debug("cant parse envelope of mail id %d (%d)", msg->msg_index, ret);
    goto leave;
  } else if ((ret = get_stdheader_info(msg, &addi))) {
    debug("cant find recipients in header of mail id %d (%d)", msg->msg_index,
      ret);
  }

  if (config.recipheader) {
    /* replace recip list, we keep the sender */
    clist_free_withcontent(addi->recipients);
    addi->recipients = clist_new();
    ret = get_userheader_recips(msg, config.recipheader, &addi);
  }

  if (!ret && (ret = mailmessage_fetch(msg, &data, &datalen))) {
    debug("cant fetch mail body  of mail id %d (%d)", msg->msg_index, ret);
  } else {
    ret = deliver_lmtp(data, datalen, addi, NULL);
  }

leave:
  if (addi)
    addinfo_free(addi);
  if (data)
    mailmessage_fetch_result_free(msg, data);
  return ret;
}

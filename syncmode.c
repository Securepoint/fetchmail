#include "sp_fetchmail.h"
#include <libspcommon/openreadclose.h>
#include <libspcommon/openwriteclose.h>
#include <libspcommon/pid_functions.h>
#include <libspcommon/sp_filefuncs.h>
#include <libspcommon/sp_string.h>

#include <errno.h>
#include <resolv.h>
#include <stdlib.h>
#include <string.h>

const char *SUPERSEPERATOR = "---------------------------------";
strbuf_t globalerrbuf = STRBUF_ZERO;

int append_msglist(struct mailmessage_list *msglist, char *boxname)
{
  unsigned int i, percent = 0, mailsdone = 0;
  int ret = 0;
  strbuf_t quotebuf = STRBUF_ZERO;

  if (MAIL_NO_ERROR != mailsession_noop(target_storage->sto_session)) {
    if (get_target_storage()) {
      syslog(LOG_ERR, "Error Connection broken");
      strbuf_put(&globalerrbuf,
        "The connection to the UMA's IMAP server was broken.");
      goto leave;
    }
  }

  if (MAIL_NO_ERROR != mailsession_select_folder(target_storage->sto_session,
      "import")) {
    debug("cant select import folder");
    goto leave;
  }
  strbuf_put(&quotebuf, boxname);
  strbuf_quote(&quotebuf, NULL);
  for (i = 0; i < carray_count(msglist->msg_tab); i++) {
    mailmessage *msg;
    char *data;
    size_t datalen;

    if (percent != ((i * 100) / carray_count(msglist->msg_tab))) {
      percent = (i * 100) / carray_count(msglist->msg_tab);
      if (config.verbose) {
        printf
          ("{\"status\":\"fetching\",\"folder\":\"%s\",\"progress\": %d}\n",
          quotebuf.buf, percent);
        fflush(stdout);
      }
      proctitle("fetching from %s: %d%%", boxname, percent);
    }
    if ((msg = (mailmessage *) carray_get(msglist->msg_tab, i))) {
      if ((config.mailsafter)
        && (!mailmessage_fetch_envelope(msg, &msg->msg_fields))
        && (msg->msg_fields)) {
        mailmessage_resolve_single_fields(msg);
        /* check for corrupted mailheaders */
        if (msg->msg_single_fields.fld_orig_date) {
          if (0 >
            mailimf_date_time_comp_noint(msg->
              msg_single_fields.fld_orig_date->dt_date_time,
              config.mailsafter)) {
            debug("skipping mail. its too old");
            mailsdone++;
            continue;
          }
          if (config.mailsbefore) {
            if (0 <
              mailimf_date_time_comp_noint(msg->
                msg_single_fields.fld_orig_date->dt_date_time,
                config.mailsbefore)) {
              debug("skipping mail. its too new");
              mailsdone++;
              continue;
            }
          }
        }
      }
      if (MAIL_NO_ERROR == mailmessage_fetch(msg, &data, &datalen)) {
        /* skip empty mails #5021 */
        if (0 == datalen) {
          syslog(LOG_INFO, "skipping zero size mail with ID %i",
            msg->msg_index);
          mailmessage_fetch_result_free(msg, data);
          mailsdone++;
          continue;
        }
        ret =
          mailsession_append_message(target_storage->sto_session, data,
          datalen);
        if (ret) {
          syslog(LOG_INFO, "append mail returned: %i", ret);
          // break here. mails wont be delivered if mails in temp differ from remote
          strbuf_putf(&globalerrbuf,
            "There was an error appending mail on the UMA temp storage. license problem or connection broken");
          goto leave;
        } else {
          mailsdone++;
        }
        mailmessage_fetch_result_free(msg, data);
      } else {
        /* Exchange HACK and when I say Hack.....
         *  -check if empty Mail has some kind of Header
         *  -and the content from Exchange Server */
        char *result = NULL;
        size_t len;

        debug("Error fetching mail. checking for M$ calender entry...");
        ret = mailmessage_fetch_header(msg, &result, &len);
        if (!ret && result) {
          if (strstr(result, "From: Microsoft Exchange Server")) {
            debug(".....seems to be. will ignore this one");
            mailsdone++;
          } else {
            debug("...its not. Error: cant fetch mail.");
            strbuf_putf(&globalerrbuf,
              "Error, the mail content can't be fetched. The email header follows: %s",
              result);
            goto leave;
          }
          mailmessage_fetch_result_free(msg, result);
        } else {
          syslog(LOG_INFO, "%s%s: Error: cant fetch mail #%d. ERR: %s",
            config.user, boxname, msg->msg_index, maildriver_strerror(ret));
          strbuf_putf(&globalerrbuf,
            "Error, the email and header can't be fetched. mail #%d (%d)",
            msg->msg_index, ret);
          goto leave;
        }
      }
    }
  }
  proctitle("fetching from %s: done", boxname);
  if (config.verbose) {
    printf("{\"status\":\"fetching\",\"folder\":\"%s\",\"progress\": 100}\n",
      quotebuf.buf);
    fflush(stdout);
  }

leave:
  strbuf_free(&quotebuf);
  return mailsdone;
}

/* takes an base64-encoded list of mailboxes seperated by '\n' */
int do_selective_sync(void)
{
  size_t buflen = strlen(targetconfig.boxlist);
  int offset = 0;
  //unsigned char *buf = calloc(1, buflen + 1);
  char *buf = calloc(1, buflen + 1);
  char *seperator, *boxname;
  struct mailmessage_list *msglist = NULL;
  int mailstodo = 0, mailsdone = 0;
  struct mail_list *localfolders;
  clistiter *listiter;

  if (-1 == __b64_pton(targetconfig.boxlist, (unsigned char *) buf, buflen)) {
    syslog(LOG_INFO, "Error decoding base64list: %s to: %s",
      targetconfig.boxlist, buf);
    if (config.verbose) {
      printf("{ \"errormsg\" : \"error decoding base64 string\" }");
      fflush(stdout);
    }
    return -1;
  }

  localfolders = get_mailboxlist(source_storage);

  seperator = boxname = buf;
  while ((seperator = memchr(seperator, '\n', buflen - offset))) {
    int hasfolder = 0;
    *seperator = '\0';
    offset = seperator - buf;
    if (!memcmp(boxname, "POP3", 4)) {
      //mailsession_get_messages_list(source_storage->sto_session, &msglist);
      if ((msglist = get_messages(source_storage, NULL))) {
        mailstodo += carray_count(msglist->msg_tab);
        mailsdone += append_msglist(msglist, "POP3");
        mailmessage_list_free(msglist);
      }
    } else {
      for (listiter = clist_begin(localfolders->mb_list); listiter;
        listiter = clist_next(listiter)) {
        char *tmpfolder = (char *) clist_content(listiter);
        if (!strncasecmp(boxname, tmpfolder, strlen(boxname))) {
          hasfolder = 1;
          break;
        }
      }

      if (hasfolder && (msglist = get_messages(source_storage, boxname))) {
        mailstodo += carray_count(msglist->msg_tab);
        mailsdone += append_msglist(msglist, boxname);
        mailmessage_list_free(msglist);
      }
    }
    boxname = seperator + 1;
  }
  if (localfolders)
    mail_list_free(localfolders);
  free(buf);
  if (config.verbose) {
    strbuf_quote(&globalerrbuf, NULL);
    printf
      ("{ \"errormsg\" : \"%s\" ,\"successfulmails\" : %u , \"errormails\" : %u, \"ignored\": %u }",
      0 < globalerrbuf.len ? globalerrbuf.buf : "none", mailsdone,
      mailstodo - mailsdone, 0);
    fflush(stdout);
    strbuf_setlength(&globalerrbuf, 0);
  }
  syslog(LOG_INFO, "%s: did %u mails. %u mails had errors. %u ignored",
    targetconfig.user, mailsdone, mailstodo - mailsdone, 0);

  /*
   * if (mailstodo == mailsdone) {
   * copy_temp_to_original();
   * } else {
   * syslog(LOG_ERR,
   * "Error occured on fetching for %s. will skip copying mails (%i/%i)",
   * config.user, mailsdone, mailstodo);
   * if (config.verbose) {
   * strbuf_quote(&globalerrbuf, NULL);
   * printf
   * ("{ \"errormsg\" : \"%s\" }",
   * (0 <
   * globalerrbuf.len) ? globalerrbuf.buf :
   * "Couldn't fetch|append all of the emails. The mail delivery stage will be skipped.");
   * fflush(stdout);
   * strbuf_setlength(&globalerrbuf, 0);
   * }
   * }
   */
  return (mailstodo - mailsdone);
}

/* #2233
 * reads todo files in DEFAULT_CACHEDIR/supersync/{users,folders,pass,server}
 */
void do_supersync()
{
  char *users = NULL, *folders = NULL, *pass = NULL, *server =
    NULL, *timerange = NULL;
  char *seperator;
  strbuf_t errbuf = STRBUF_ZERO, folderbuf = STRBUF_ZERO;
  int len, userslen;
  int dolist = 0;
  clist *folderlist = NULL;
  clistiter *iter;

  if (chdir(DEFAULT_CACHEDIR))
    errorout("cant change to homedir");
  if (access("supersync", R_OK | W_OK | X_OK))
    errorout
      ("The working directory does not exist, or the permissions are invalid.");
  if (chdir("supersync"))
    errorout
      ("The working directory does not exist, or the permissions are invalid.");

  if (syncmode_superheftig_list == config.syncmode) {
    folderlist = clist_new();
    dolist = 1;
  }

  if ((!dolist) && openreadclose("folders", &folders, &len)) {
    errorout("The folders file could not be found.");
  }
  if (!access("timerange", R_OK)) {
    if (!openreadclose("timerange", &timerange, &len)) {
      if (parse_timerange(timerange))
        errorout("The time range could not be parsed.");
      free(timerange);
    }
  }

  if (openreadclose("pass", &pass, &len)) {
    errorout("The pass file could not be found.");
  } else if (openreadclose("server", &server, &len)) {
    errorout("The server file could not be found.");
  } else if (openreadclose("users", &users, &userslen)) {
    /* spdev/juanp/JohnDoe|johnd */
    errorout("The users file could not be found.");
  }
  if (0 > write_pidfile("pid")) {
    if (EEXIST == errno)
      errorout("There is already an instance of the import tool running.");
    errorout("cant write pid file");
  }

  targetconfig.boxlist = folders;
  config.password = pass;
  config.server = server;
  // see #5379
  config.boxtype = IMAP_STORAGE_BASIC;

  seperator = users;

  // see #5443
  if (!dolist)
    printf("%s\n", users);
  /* iterate over the userlist */
  while ((users) && (seperator = memchr(seperator, '\n', strlen(seperator)))) {
    int ret = 1, linelen;
    char *samauser;
    *seperator = '\0';
    linelen = strlen(users);
    /* split MailNickName and SamaAccountName */
    if (!(samauser = memchr(users, '|', linelen))) {
      errorout("There was an error parsing the sAMAccountName.");
    }
    // check for mailnickname #6572
    if (!memcmp(samauser - 1, "/", 1)) {
      strbuf_appendf(&errbuf,
        "No mailnickname attribut for user %s\n", samauser);
      goto cont;
    }
    *samauser = '\0';
    samauser++;
    targetconfig.user = samauser;
    config.user = users;

    if (!dolist) {
      printf("%s%s|%s\n", SUPERSEPERATOR, users, samauser);
      fflush(stdout);
    }

    if (get_source_storage()) {
      strbuf_appendf(&errbuf,
        "Could not log in to the remote server as the user %s|%s\n",
        config.user, samauser);
      if (!dolist)
        printf
          ("{ \"errormsg\" : \"Could not log in to the remote server as the user %s|%s\" ,\"successfulmails\" : 0 , \"errormails\" : 0, \"ignored\": 0 }",
          config.user, samauser);
      goto cont;
    }
    if ((!dolist) && get_target_storage()) {
      strbuf_appendf(&errbuf, "cant login on uma. skipping user %s\n",
        targetconfig.user);
      printf
        ("{ \"errormsg\" : \"cant login on uma. skipping user %s\" ,\"successfulmails\" : 0 , \"errormails\" : 0, \"ignored\": 0 }",
        targetconfig.user);
      goto cont;
    }
    if ((!dolist) && (ret = do_selective_sync())) {
      strbuf_appendf(&errbuf, "Errors occured. skipping user %s\n",
        targetconfig.user);
    } else if (dolist) {
      /* append the folder */
      append_folderlist(folderlist);
    }

  cont:
    if (!dolist) {
      printf("\n%s%s|%s\n", SUPERSEPERATOR, users, samauser);
      fflush(stdout);
    }
    userslen -= (linelen + 1);
    if (0 < userslen) {
      users = ++seperator;
    } else {
      users = NULL;
    }
    if (!dolist) {
      if (!ret) {
        /* write new users file without the successfully done user */
        if (0 > creatwriteclose("users", users, S_IRUSR | S_IWUSR))
          strbuf_append(&errbuf, "wrong permissions on working folder\n");
      }
      if (errbuf.len) {
        strbuf_quote(&errbuf, NULL);
        openwriteclose("error", errbuf.buf, O_WRONLY | O_APPEND | O_CREAT,
          0666);
        strbuf_setlength(&errbuf, 0);
      }
      if (globalerrbuf.len) {
        strbuf_quote(&globalerrbuf, NULL);
        openwriteclose("error", globalerrbuf.buf,
          O_WRONLY | O_APPEND | O_CREAT);
      }
    }

    /* close our connections */
    if (source_storage) {
      mailstorage_free(source_storage);
      source_storage = NULL;
    }
    if (target_storage) {
      mailstorage_free(target_storage);
      target_storage = NULL;
    }
  }                             // END user iteration

  if ((!dolist) && (access("error", F_OK))) {
    /* write done file when finished without errors */
    creatwriteclose("done", "", 0666);
  } else if (dolist) {
    strbuf_t quotebuf = STRBUF_ZERO;
    strbuf_put(&folderbuf, "{\"mailboxes\":[");
    for (iter = clist_begin(folderlist); iter; iter = clist_next(iter)) {
      Foldercount *it = clist_content(iter);
      strbuf_put(&quotebuf, it->name);
      strbuf_quote(&quotebuf, NULL);
      strbuf_appendf(&folderbuf, "{\"name\":\"%s\",\"count\":\"%d\"}%s",
        quotebuf.buf, it->count, clist_next(iter) ? "," : " ");
    }
    strbuf_append(&folderbuf, "]");
    if (errbuf.len) {
      strbuf_quote(&errbuf, NULL);
      strbuf_appendf(&folderbuf, ",\"error\":\"%s\"", errbuf.buf);
    }
    strbuf_append(&folderbuf, "}");
    strbuf_write(&folderbuf, 1);
  }
  exit(0);
}

void syncbox(void)
{
  struct mail_list *mailboxes = NULL;
  struct mailmessage_list *msglist;
  clistiter *iter;

  if (syncmode_copyonly == config.syncmode) {
    printf("{ \"errormsg\" : \"DEPRECATED!\"}");
    // this mode is deprecated since uma3
    return;
  }
  if (syncmode_superheftig == config.syncmode
    || syncmode_superheftig_list == config.syncmode) {
    do_supersync();
    return;
  }

  if (get_source_storage()) {
    if (config.verbose) {
      printf("{ \"errormsg\" : \"wrong credentials?\"}");
      fflush(stdout);
    }
    return;
  }
  /* just print a list of remote folders in json */
  if (syncmode_list_only == config.syncmode) {
    printf("{\"mailboxes\":[");
  } else {
    if (get_target_storage()) {
      if (config.verbose) {
        printf("{ \"errormsg\" : \"cant login on uma\"}");
        fflush(stdout);
      }
      return;
    }
    if (targetconfig.boxlist) {
      do_selective_sync();
      return;
    }
  }

  if ((mailboxes = get_mailboxlist(source_storage))) {
    strbuf_t quotebuf = STRBUF_ZERO;
    for (iter = clist_begin(mailboxes->mb_list); iter != NULL;
      iter = clist_next(iter)) {
      char *boxname = (char *) clist_content(iter);
      uint32_t messagecount = 0;
      mailsession_messages_number(source_storage->sto_session, boxname,
        &messagecount);
      if (syncmode_list_only == config.syncmode) {
        strbuf_put(&quotebuf, boxname);
        strbuf_quote(&quotebuf, NULL);
        printf("{\"name\":\"%s\",\"messages\":\"%u\"}%s", quotebuf.buf,
          messagecount, clist_next(iter) ? "," : " ");
      } else if (0 < messagecount) {
        if ((msglist = get_messages(source_storage, boxname))) {
          syslog(LOG_INFO, "Pushing %i mails in folder %s.", messagecount,
            boxname);
          append_msglist(msglist, boxname);
          mailmessage_list_free(msglist);
        }
      }
    }
    mail_list_free(mailboxes);
    strbuf_free(&quotebuf);
  } else {
    /* no folders (POP3) push all in INBOX */
    if (syncmode_all == config.syncmode) {
      if (MAIL_NO_ERROR !=
        mailsession_get_messages_list(source_storage->sto_session,
          &msglist)) {
        append_msglist(msglist, "INBOX");
        mailmessage_list_free(msglist);
      }
    }
    uint32_t messagecount = 0;
    mailsession_messages_number(source_storage->sto_session, "",
      &messagecount);
    printf("{\"name\":\"POP3\",\"messages\":\"%u\"}", messagecount);
  }

  if (syncmode_list_only == config.syncmode) {
    printf("]}");
    fflush(stdout);
  }

  return;
}

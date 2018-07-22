#include "sp_fetchmail.h"

#include <string.h>
#include <stdlib.h>

#include <sys/types.h>          //open waitpid
#include <sys/wait.h>           //waitpid
#include <sys/prctl.h>          //prctl
#include <unistd.h>             //fork
#include <errno.h>

/* writes all messages in msglist to stdout
 * or to forked qmail-inject 
 * -returns the number of processed messages */
int print_out_mails(struct mailmessage_list *msglist, int unseenmails)
{
  mailmessage *msg;
  unsigned int i;
  int totalcount = 0, ret, checkall = 0;

  if (!msglist)
    return -1;

  /* iterate over messaglist */
  i = carray_count(msglist->msg_tab) - unseenmails;
checkall:
  for (; i < carray_count(msglist->msg_tab); i++) {
    ret = 1;
    if ((msg = (mailmessage *) carray_get(msglist->msg_tab, i))) {
      /* get the flags of the mail */
      mailmessage_get_flags(msg, &msg->msg_flags);
      /* get msg if no flags (pop3) or if Flag is UNSEEN */
      if (((!msg->msg_flags) || !(MAIL_FLAG_SEEN & msg->msg_flags->fl_flags))) {
        if ((config.maxsize) && ((int) msg->msg_size > config.maxsize)) {
          syslog(LOG_WARNING,
            "skipping message uid %s. it exceeds maximum size of %d bytes / %d",
            msg->msg_uid, config.maxsize, (int) msg->msg_size);
          syslog(LOG_WARNING,
            "will set SEEN flag. so please check this mail manually");
          if (msg->msg_flags) {
            msg->msg_flags->fl_flags |= MAIL_FLAG_SEEN;
            mailmessage_check(msg);
          }
        } else if ((!config.disableenvelope)
          && (0 == (ret = check_for_msjournal(msg)))) {
          /* doit */
        } else if (0 == (ret = check_header(msg, NULL))) {
          /* doit */
        } else {
          debug
            ("error fetching mail: msg_uid=%s, errormsg: %s (%d)\n",
            msg->msg_uid, maildriver_strerror(ret), ret);
        }
        /* if successful and not syncmode, set SEEN flag and remove mail if !keepmails */
        if (!ret) {
          totalcount++;
          if (msg->msg_flags && (!config.syncmode))
            msg->msg_flags->fl_flags |= MAIL_FLAG_SEEN;
          /* store flags in session */
          mailmessage_check(msg);
          if (!config.keepmails)
            mailsession_remove_message(source_storage->sto_session,
              msg->msg_index);
        }
      }
      mailmessage_flush(msg);
    }
  }
  if (!checkall && config.keepmails && totalcount != unseenmails) {
    i = 0;
    debug("have to check all mails in folder. (unseen:%d|total:%d)\n",
      unseenmails, totalcount);
    checkall = 1;
    goto checkall;
  }
  mailmessage_list_free(msglist);
  return totalcount;
}

/* a.k.a main */
void fetchit()
{

  struct mail_list *mailboxes = NULL;
  struct mailmessage_list *msglist;
  clistiter *iter;
  clist *pop3list = NULL;
  int totalmails = 0, evilmailsfd = -1;
  uint32_t totalmessagecount = 0;

  if (get_source_storage())
    return;

  proctitle("fetching mails for %s from %s", config.user, config.server);

  evilmails = clist_new();
  clist_readfromfile(evilmails, EVILMAILSFILE, "\n");

  if (!(mailboxes = get_mailboxlist(source_storage))) {
    /* no folder structure => POP3  create a folderlist with a single folder */
    pop3list = clist_new();
    clist_append(pop3list, strdup("TYPEPOP3"));
    mailboxes = mail_list_new(pop3list);
  }

  /* iterate over all mailboxes */
  for (iter = clist_begin(mailboxes->mb_list);
    iter != NULL; iter = clist_next(iter)) {
    char *boxname = (char *) clist_content(iter);
    uint32_t messagecount, unseen, recent;

    /* get info about messages and flags in boxname */
    if (MAIL_NO_ERROR ==
      mailsession_status_folder(source_storage->sto_session, boxname,
        &messagecount, &recent, &unseen)) {
      if ((0 < unseen) || ((config.syncmode) && (messagecount > 0))) {
        debug("will do %d messages in folder %s\n",
          config.syncmode ? messagecount : unseen, boxname);
        totalmessagecount += (config.syncmode ? messagecount : unseen);
        if (memcmp(boxname, "TYPEPOP3", 8)) {
          if (mailsession_select_folder(source_storage->sto_session, boxname)) {
            syslog(LOG_ERR, "[%s] error selecting folder %s.",
              config.user, boxname);
            continue;
          }
        }
        if ((msglist = get_messages(source_storage, NULL))) {
          /* iterate over all messages in boxname */
          totalmails += print_out_mails(msglist, unseen);
          /* TODO: check if this is really needed to store our flags 
           * =>  Yes it is!                                   */
          mailsession_check_folder(source_storage->sto_session);
        } else {
          syslog(LOG_ERR, "[%s] error getting maillist from folder %s.",
            config.user, boxname);
        }
      } else {
        debug("no messages to fetch in folder %s", boxname);
      }
    } else {
      debug("error getting status information from %s. skipping folder",
        boxname);
    }
  }
  if (totalmails != (int) totalmessagecount) {
    syslog(LOG_INFO, "[%s] done with errors: fetched %d from %d totalmails.",
      config.user, totalmails, totalmessagecount);
  } else if (totalmails) {
    syslog(LOG_INFO, "[%s] done: sucessfully fetched %d mails", config.user,
      totalmails);
    mailsmtp_free(lmtp_storage);
    lmtp_storage = NULL;
  } else
    syslog(LOG_INFO, "[%s] done: nothing new to be fetched.", config.user);

  if (mailboxes)
    mail_list_free(mailboxes);

  mailstorage_free(source_storage);
  source_storage = NULL;
  if (0 < clist_count(evilmails)) {
    if (0 < (evilmailsfd = open(EVILMAILSFILE, O_RDWR | O_CREAT, 0666))) {
      clist_printout(evilmails, evilmailsfd, "\n");
      close(evilmailsfd);
    }
  } else {
    unlink(EVILMAILSFILE);
  }

  return;
}

/*to prevent etpan / openssl memleaks*/
void fork_and_fetchit()
{
  pid_t pid = -1;
  int child_status = -1;
  int wret = -1;
  pid = fork();
  if (pid == -1) {
    syslog(LOG_ERR, "Fork error.");
    return;
  }
  /*child */
  if (!pid) {
    errno = 0;
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (errno) {
      syslog(LOG_ERR, "Error unable to set prctl.");
      _exit(0);
    }
    fetchit();
    _exit(0);                   /*dont call atexit funcs */
  }
  /*parent */
  wret = waitpid(pid, &child_status, 0);
  if (wret == pid && WIFEXITED(child_status)) {
    return;
  }                             //aright
  syslog(LOG_ERR, "Error getting child status. Sending kill signal");
  kill(pid, 9);
}

int main(int argc, char **argv)
{

  int c;
  struct sigaction sigset;

  source_storage = NULL;
  target_storage = NULL;
/* set our global defaults */
  config.qmailstyle = 0;
  config.verbose = 0;
  config.debug = 0;
  config.disableenvelope = 0;
  config.syncmode = syncmode_nothing;
  config.cachedir = NULL;
  config.pop3flagdir = NULL;
  config.tmpdir = NULL;
  config.mailsbefore = NULL;
  config.mailsafter = NULL;
  config.archive_domains = NULL;

  targetconfig.boxlist = NULL;
  targetconfig.user = NULL;

  free_config();

  atexit(cleanup);
  sigemptyset(&sigset.sa_mask);
  sigset.sa_flags = SA_RESTART;
  sigset.sa_handler = fetchmailsighandler;
  sigaction(SIGPIPE, &sigset, NULL);

  while ((c = getopt(argc, argv, "c:t:u:p:s:P:i:S:U:C:D:d::Tvb")) != -1) {
    /* we have to strdup char* cuz we use proctitle */
    switch (c) {
     case 't':
       if (!memcmp(optarg, "POP3", 4)) {
         config.boxtype = POP3_STORAGE;
       } else if (!memcmp(optarg, "IMAP", 4)) {
         config.boxtype = IMAP_STORAGE;
       } else {
         config.boxtype = PROTO_AUTO;
       }
       break;
     case 'u':
       config.user = strdup(optarg);
       break;
     case 'p':
       config.password = strdup(optarg);
       break;
     case 's':
       config.server = strdup(optarg);
       break;
     case 'P':
       config.port = atoi(optarg);
       break;
     case 'i':
       config.interval = atoi(optarg);
       break;
     case 'T':
       config.connection_type = CONNECTION_TYPE_TLS;
       break;
     case 'v':
       config.verbose = 1;
       break;
     case 'd':
       config.debug = 1;
       if (optarg && !memcmp(optarg, "d", 1)) {
         unlink("libetpan-stream-debug.log");
         mailstream_debug = 1;
       }
       break;
     case 'b':
       config.connection_type = CONNECTION_TYPE_PLAIN;
       break;
     case 'S':
       if (!memcmp(optarg, "list", 4)) {
         config.syncmode = syncmode_list_only;
       } else if (!memcmp(optarg, "all", 3)) {
         config.syncmode = syncmode_all;
       } else if (!memcmp(optarg, "copyonly", 8)) {
         config.syncmode = syncmode_copyonly;
       } else if (!memcmp(optarg, "superlist", 9)) {
         config.syncmode = syncmode_superheftig_list;
       } else if (!memcmp(optarg, "super", 5)) {
         config.syncmode = syncmode_superheftig;
       } else {
         config.syncmode = syncmode_selective;
         targetconfig.boxlist = strdup(optarg);
       }
       break;
     case 'U':
       targetconfig.user = strdup(optarg);
       break;
     case 'C':
       config.cachedir = strdup(optarg);
       break;
     case 'D':
       if (!optarg)
         usage();
       if (parse_timerange(optarg)) {
         printf("\nerror parsing time range\n");
         usage();
       }
       break;
     case 'c':
       if (optarg) {
         /* we will never return */
         openlog(argv[0], LOG_CONS, LOG_MAIL);
         use_configfile(optarg);
       } else
         usage();
       break;
     default:
       usage();
    }
  }

  openlog("fetchmail-mailimport", LOG_CONS, LOG_MAIL);
  initproctitle(argc, argv);

    proctitle("starting up...");
/* we get here only when working from command line (no "-c" given) */
    if (config.syncmode) {
      syncbox();
    } else {
      if (!config.user || !config.password || !config.server
        || 0 > config.boxtype)
        usage();

      while (1) {
        /* do it */
        fork_and_fetchit();
        if (config.interval) {
          sleep(config.interval);
        } else
          return 0;
      }
    }
  return 0;
}

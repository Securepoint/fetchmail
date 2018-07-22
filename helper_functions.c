#include "sp_fetchmail.h"
#include <libspcommon/openreadclose.h>
#include <libspcommon/sp_filefuncs.h>
#include <libspuma-common/spuma-license.h>

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

const char const *fetchmailversion = VERSION;

int lmtp_check_connection()
{
  int ret = 0;

  if (lmtp_storage) {
    if (MAILSMTP_NO_ERROR == mailsmtp_reset(lmtp_storage))
      return 0;
    mailsmtp_free(lmtp_storage);
  }
  if ((lmtp_storage = mailsmtp_new(0, NULL)) == NULL) {
    debug("oom allocating lmtp handle");
    return -1;
  }
  if ((ret = mailsmtp_socket_connect(lmtp_storage,
        (DOVECOT_LMTP_SOCKET), 24)) != MAILSMTP_NO_ERROR) {
    syslog(LOG_ERR, "cant connect to lmtp server (%s)",
      mailsmtp_strerror(ret));
    return -2;
  }
  return ret;
}

void cleanup()
{
  syslog(LOG_INFO, "...exiting");
  /* be nice and say goodbye */
  if (source_storage)
    mailstorage_free(source_storage);

  if (config.tmpdir)
    sp_rrmdir(config.tmpdir);

  if (target_storage) {
    mailstorage_free(target_storage);
  }
}

void fetchmailsighandler(int sig)
{
  switch (sig) {
   case (SIGPIPE):
     debug("got a SIGPIPE");
     break;
   default:
     debug("got SIGNAL %d", sig);
     break;
  }
  return;
}

void usage(void)
{
  int compilerwarn;
  const char *msg =
    "\nuse -c and ini style config file(see example.conf) OR command line\ncapital letter options are optional\nusage:\n sp_fetchmail -t [POP3|IMAP|AUTO] -u [user] -p [password] -s [server] -P [port] -T(enable TLS port 995|993) -v(verbose json output) -d(ebug) -b(use blain connection) -D 1397318400[-1403625600](only fetch mails from this timerange) -S [list|all|b64encoded list|super|superlist](import mails to UMA) -U [UMA-Username]\n";
  compilerwarn = write(2, "fetchmail version:", 18);
  compilerwarn = write(2, fetchmailversion, strlen(fetchmailversion));
  compilerwarn = write(2, msg, strlen(msg));
  exit(compilerwarn);
}

void debug_real(const char *func, const char *format, ...)
{
  va_list args;
  if (1 < config.debug)
    syslog(LOG_DEBUG, "func=%s", func);
  va_start(args, format);
  if (config.debug)
    vsyslog(LOG_DEBUG, format, args);
  va_end(args);
}

void errorout(const char *output)
{
  strbuf_t errbuf = STRBUF_ZERO;
  if (config.verbose) {
    strbuf_putf(&errbuf, "{ \"error\" : \"%s\"}", output);
    strbuf_write(&errbuf, 2);
  } else {
    printf("%s\n", output);
  }
  syslog(LOG_ERR, "%s", output);
  exit(1);
}

// #3758
int parse_timerange(const char *range)
{
  char *tmp;
  long longtmp;
  int ret = 0;
  struct tm *tmtmp;

  /* check if a range(from-to) is given */
  if ((tmp = memchr(range, '-', strlen(range)))) {
    *tmp = '\0';
    longtmp = atol(tmp + 1);
    if ((tmtmp = localtime(&longtmp))) {
      config.mailsbefore =
        mailimf_date_time_new(tmtmp->tm_mday, (tmtmp->tm_mon + 1),
        (tmtmp->tm_year + 1900), tmtmp->tm_hour, tmtmp->tm_min,
        tmtmp->tm_sec, 0);
    } else {
      ret = 1;
    }
  }

  longtmp = atol(range);
  if ((tmtmp = localtime(&longtmp))) {
    config.mailsafter =
      mailimf_date_time_new(tmtmp->tm_mday, (tmtmp->tm_mon + 1),
      (tmtmp->tm_year + 1900), tmtmp->tm_hour, tmtmp->tm_min,
      tmtmp->tm_sec, 0);
  } else {
    ret = 1;
  }
  return ret;
}

int forkandwrite_extprogram(char *const argv[], char *data, size_t len)
{

  int pipefd[2], pipefdlog[2];
  int pid = -1, status = -1;
  char errbuf[255] = { 0 };

  if (pipe(pipefd))
    return -1;
  if (pipe(pipefdlog))
    return -1;

  switch (pid = fork()) {
   case 0:
     dup2(pipefd[0], STDIN_FILENO);
     dup2(pipefdlog[1], STDERR_FILENO);
     close(pipefd[1]);
     close(pipefdlog[0]);
     execv(argv[0], argv);
     _exit(1);
     break;
   case -1:
     syslog(LOG_ERR, "error on forking. OOM?");
     if (!config.syncmode) {
       syslog(LOG_ERR, "will exit and try on next run");
       sleep(1);
       exit(-1);
     }
     break;
   default:
     close(pipefd[0]);
     close(pipefdlog[1]);
     if ((ssize_t) len != (write(pipefd[1], data, len)))
       debug("Error writing to %s child", argv[0]);
     close(pipefd[1]);
     wait(&status);
  }
  /* catch stderr on err #4538 */
  if (WEXITSTATUS(status)) {
    len = read(pipefdlog[0], &errbuf, 255);
    /* remove linebreaks from syslog message */
    while (len--) {
      if (('\n' == errbuf[len]) || ('\r' == errbuf[len]))
        errbuf[len] = ' ';
    }
    debug("%s said: %s", argv[0], errbuf);
  }
  close(pipefdlog[0]);

  return (WEXITSTATUS(status));
}

int forkandwrite_qmailinject(char *data, size_t len)
{

  char *argv[] = { "/opt/spqmail-in/bin/qmail-inject", NULL };
  return (forkandwrite_extprogram((char *const *) argv, data, len));
}

/* not exported just for lmtp delivery */
char *add_destfolder_to_user(char *recip, const char *folder)
{
  char *tmp;
  strbuf_t tmpbuf = STRBUF_ZERO;

  /* username+Sent || userpart+Sent@domain.org */
  if ((tmp = rindex(recip, '@'))) {
    strbuf_nappend(&tmpbuf, recip, (tmp - recip));
    strbuf_append(&tmpbuf, "+");
    strbuf_append(&tmpbuf, folder);
    strbuf_append(&tmpbuf, tmp);
  } else {
    strbuf_put(&tmpbuf, recip);
    strbuf_append(&tmpbuf, "+");
    strbuf_append(&tmpbuf, folder);
  }
  return tmpbuf.buf;
}

/*
uses the global lmtp connection(reopened on demand)
and writes recipients, sender and mailbody(data) to uma lmtp connection

if folder is given, its appended to the userstring after the
recipient delimiter
*/
int deliver_lmtp(char *data, size_t datalen,
  addressinfo_t * addi, const char *folder)
{
  clistiter *iter;
  clist *destusers;
  int ret, i, recpc = 0;
  int *retcodes = NULL;
  char *tmp, *recipient;

  if (lmtp_check_connection()) {
    return -2;
  }

  destusers = addi->recipients;

  /* check if sender is local and has to be archived in Sent Folder */
  if (!config.syncmode && (0 < isin_archive_domains(addi->sender))) {
    if ((tmp = add_destfolder_to_user(addi->sender, "Sent"))) {
      clist_uniadd(destusers, tmp);
      free(tmp);
    } else {
      debug("error adding folder to recipient");
    }
  }

  /* say hello and check recipients */
  if ((ret =
      mailesmtp_lhlo(lmtp_storage, "sp-fetchmail")) != MAILSMTP_NO_ERROR) {
    debug("lhlo error");
    goto exit;
  } else if ((ret = mailesmtp_mail(lmtp_storage, "sp-fetchmail", 0, NULL))) {
    debug("mail from error");
    goto exit;
  }
  for (iter = clist_begin(destusers); iter; iter = clist_next(iter)) {
    if (folder) {
      recipient = add_destfolder_to_user(clist_content(iter), folder);
    } else {
      recipient = clist_content(iter);
    }
    if (0 == isin_archive_domains(recipient)) {
      syslog(LOG_INFO, "recipient %s not in our domainlist", recipient);
      free(clist_content(iter));
      clist_delete(destusers, iter);
      continue;
    }
    if ((ret = mailesmtp_rcpt(lmtp_storage, recipient, 0, NULL))) {
      if (MAILSMTP_ERROR_MAILBOX_UNAVAILABLE == ret) {
        syslog(LOG_INFO, "recipient %s not available, or denied by license",
          recipient);
        ret = 0;
      } else if (MAILSMTP_ERROR_IN_PROCESSING == ret) {
        // ex. ldap not accesible
        syslog(LOG_ERR, "temp error on %s. will skip this run (%d)",
          recipient, ret);
        ret = 1;
        goto exit;
      } else {
        syslog(LOG_ERR, "recipient %s gave error. will skip it (%d)",
          recipient, ret);
      }
      /* free element AND its content from list */
      free(clist_content(iter));
      clist_delete(destusers, iter);
    } else {
      recpc++;
    }
    if (folder)
      free(recipient);
  }

  // TODO: check all kinds of errors from RCPT TO: ldap,disk etc. not accessible
  // should match (0 == recpc) and return error to caller

  /* deliver mail-body */
  if (0 == recpc) {
    debug("no recipients found");
  } else if ((ret = mailsmtp_data(lmtp_storage))) {
    debug("DATA error");
  } else if (!(retcodes = malloc(sizeof(int) * clist_count(destusers) + 1))) {
    debug("oom allocating retcode array");
  } else if ((ret =
      maillmtp_data_message(lmtp_storage, data, datalen, destusers,
        retcodes))) {
    debug("DATA message error (%d)", ret);
    /* check response codes for each Recipient 
     * Errors in this place are "quota exceeded" etc. */
    /* TODO: how to act on errors for just one recip in a many recip case ? */
    for (i = 0; i < clist_count(destusers); i++) {
      if (250 != retcodes[i]) {
        debug("code for %s: %d\n", clist_nth_data(destusers, i), retcodes[i]);
        ret = retcodes[i];
      }
    }
  } else {
    /* läuft */
  }
//debug("response was: %s\n", smtp->response);
exit:
  free(retcodes);
  return ret;
}

/*
 * Usage: dovecot-lda [-c <config file>] [-a <address>] [-d <username>] [-p <path>]
 *                    [-f <envelope sender>] [-m <mailbox>] [-e] [-k]
 * http://wiki2.dovecot.org/Tools/DovecotLDA                   
 */
int forkandwrite_dovecotlda(char *data, size_t datalen,
  const char *destuser, const char *folder)
{

  const char *cmd[6];
  int i = 0;
  char *tmp;

  cmd[i++] = "/usr/libexec/dovecot/dovecot-lda";
  if (destuser) {
    cmd[i++] = "-d";
    cmd[i++] = destuser;
  }
  if (folder) {
    // change folder seperator to "/"
    while ((tmp = strstr(folder, ".")))
      *tmp = '/';

    cmd[i++] = "-m";
    cmd[i++] = folder;
  }
  cmd[i] = NULL;
  if (5 == i)
    debug("calling %s %s %s %s %s", cmd[0], cmd[1], cmd[2], cmd[3], cmd[4]);
  return (forkandwrite_extprogram((char *const *) cmd, data, datalen));
}

void free_config()
{
  /* clean up pop3 tmpdir */
  if (config.tmpdir) {
    if (!chdir(config.tmpdir))
      sp_rrmdir(config.server);
    if (config.pop3flagdir)
      free(config.pop3flagdir);
    free(config.tmpdir);
    config.pop3flagdir = config.tmpdir = NULL;
  }

  if (config.user)
    free(config.user);
  if (config.password)
    free(config.password);
  if (config.server)
    free(config.server);
  config.user = config.password = config.server = NULL;
  if (targetconfig.user)
    free(targetconfig.user);
  targetconfig.user = NULL;

  config.boxtype = -1;
  config.port = 0;
  config.connection_type = CONNECTION_TYPE_TRY_STARTTLS;
  config.keepmails = 0;
  config.maxsize = 0;
  config.interval = 0;
  if (config.recipheader)
    free(config.recipheader);
  config.recipheader = NULL;
}

/* directory for storing flags on POP3 Accounts */
void check_cachedir()
{
  strbuf_t tmpbuf = STRBUF_ZERO;

  if (!config.cachedir)
    config.cachedir = DEFAULT_CACHEDIR;

  strbuf_putf(&tmpbuf, "/%s/%s/%s", config.cachedir, config.user, "flags");
  free(config.pop3flagdir);
  config.pop3flagdir = strdup(tmpbuf.buf);
  strbuf_putf(&tmpbuf, "/%s/%s/%s", config.cachedir, config.user, "tmp");
  free(config.tmpdir);
  config.tmpdir = strdup(tmpbuf.buf);
  if ((sp_pmkdir(config.pop3flagdir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH))
    && (errno != EEXIST)) {
    debug("cant create flagdir");
    free(config.pop3flagdir);
    config.pop3flagdir = NULL;
  }

  if ((sp_pmkdir(config.tmpdir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH))
    && (errno != EEXIST)) {
    debug("cant create cachedir");
    free(config.tmpdir);
    config.tmpdir = NULL;
  }
  strbuf_free(&tmpbuf);
  return;
}

int get_source_storage()
{

  char *popcache = NULL;
  int localboxtype = config.boxtype;

proto_auto:
  if (POP3_STORAGE == localboxtype) {
    check_cachedir();
    popcache = config.tmpdir;
  } else if (PROTO_AUTO == localboxtype) {
    localboxtype = IMAP_STORAGE;
  }
  /* watch out for IMAP_BASIC PLAIN here.
   * its needed for M$ Mass Import as SASL is no worky in this
   * "special" login handling */

  if (!(source_storage =
      init_storage(localboxtype, config.server,
        config.port, config.user, config.password, config.connection_type,
        popcache, config.pop3flagdir))) {
    if ((PROTO_AUTO == config.boxtype) && (IMAP_STORAGE == localboxtype)) {
      syslog(LOG_WARNING, "Error connecting to %s via IMAP, trying POP3.",
        config.server);
      localboxtype = POP3_STORAGE;
      goto proto_auto;
    }
    syslog(LOG_ERR, "Error connecting to %s.", config.server);
    return -1;
  }
  return 0;
}

int get_target_storage()
{
  strbuf_t sysuser = STRBUF_ZERO;
  char *password = NULL;
  int len;

  if (!targetconfig.user) {
    debug("target user not set");
    return -1;
  }
  /* get masteruser password */
  strbuf_putf(&sysuser, "%s/.sys", DEFAULT_CACHEDIR);
  if (openreadclose(sysuser.buf, &password, &len)) {
    syslog(LOG_ERR, "no password on UMA");
    return 1;
  }
  /* remove linebreaks */
  if ('\n' == password[len - 1])
    password[len - 1] = '\0';

  strbuf_putf(&sysuser, "%s*sysuser", targetconfig.user);

  if (!(target_storage =
      init_storage(IMAP_STORAGE, "127.0.0.1", 0, sysuser.buf,
        password, CONNECTION_TYPE_PLAIN, NULL, NULL))) {
    syslog(LOG_WARNING, "Error connecting %s on UMA", targetconfig.user);
    free(password);
    return 1;
  }
  free(password);
  strbuf_free(&sysuser);
  return 0;
}

int append_folderlist(clist * folderlist)
{

  struct mail_list *mailboxes = NULL;
  clistiter *iter;
  clistiter *listiter;
  char *removewarn;
  int alreadyinlist;

  if (!folderlist || !source_storage)
    return 1;

  if ((mailboxes = get_mailboxlist(source_storage))) {
    for (iter = clist_begin(mailboxes->mb_list); iter;
      iter = clist_next(iter)) {
      alreadyinlist = 0;
      /* iterate over the list, check if folder is already in it */
      for (listiter = clist_begin(folderlist); listiter;
        listiter = clist_next(listiter)) {
        Foldercount *folderiter = clist_content(listiter);
        removewarn = (char *) clist_content(iter);
        if (strlen(removewarn) == strlen(folderiter->name)
          && !memcmp(folderiter->name, removewarn, strlen(removewarn))) {
          alreadyinlist = 1;
          folderiter->count++;
          break;
        }
      }
      /* not found => append new entry */
      if (!alreadyinlist) {
        Foldercount *newitem = malloc(sizeof(Foldercount));
        removewarn = (char *) clist_content(iter);
        newitem->name = strdup(removewarn);
        newitem->count = 1;
        clist_append(folderlist, newitem);
      }
    }

  } else {
    return 1;
  }
  return 0;
}

int valid_license()
{

  size_t errmsg_len = 256;
  char errmsg[errmsg_len];
  int days_left;
  time_t expire_time;
  char *license_addr = NULL;
  int account_limit;
  int err;

  err =
    spumacommon_license_check(errmsg, errmsg_len, &days_left, &expire_time,
    &license_addr, &account_limit);
  if (SPUMACOMMON_LICENSE_ACCEPT == err)
    return 1;
  return 0;
}

int clist_free_withcontent(clist * list)
{
  clistiter *iter;
  void *tmp;

  if (!list)
    return 0;

  for (iter = clist_begin(list); iter != NULL; iter = clist_next(iter)) {
    tmp = clist_content(iter);
    free(tmp);
    clist_delete(list, iter);
  }
  clist_free(list);
  list = NULL;
  return 0;
}

void addinfo_free(addressinfo_t * ai)
{
  clist_free_withcontent(ai->recipients);
  free(ai->sender);
  free(ai);
}

addressinfo_t *addinfo_new(void)
{

  addressinfo_t *newinfo = calloc(1, sizeof(addressinfo_t));
  newinfo->recipients = clist_new();
  return (newinfo);
}

/* checks the clist for an entry. if it exists returns 1 and removes the entry */
int clist_inlistremove(clist * list, char *entry)
{
  clistiter *iter;
  char *tmp;

  if (!list)
    return 0;

  /* check if entry is already present in the list */
  for (iter = clist_begin(list); iter != NULL; iter = clist_next(iter)) {
    tmp = clist_content(iter);
    if (!strcasecmp(tmp, entry)) {
      free(tmp);
      clist_delete(list, iter);
      return 1;
    }
  }
  return 0;
}

void clist_uniadd(clist * list, char *entry)
{

  clistiter *iter;
  char *tmp;

  if (!list)
    return;

  /* check if entry is already present in the list */
  for (iter = clist_begin(list); iter != NULL; iter = clist_next(iter)) {
    tmp = clist_content(iter);
    if (!strcasecmp(tmp, entry))
      return;
  }
  clist_append(list, strdup(entry));
}

void clist_printout(clist * list, int fd, const char *delim)
{
  clistiter *iter;
  char *tmp;
  strbuf_t buf = STRBUF_ZERO;

  if (!list || 0 > fd || !delim)
    return;

  for (iter = clist_begin(list); iter != NULL; iter = clist_next(iter)) {
    tmp = clist_content(iter);
    strbuf_appendf(&buf, "%s%s", tmp, delim);
  }
  strbuf_write(&buf, fd);
  strbuf_free(&buf);
}

int clist_readfromfile(clist * list, const char *filename, const char *delim)
{
  strbuf_t filecontent = STRBUF_ZERO;
  char *entry;
  int ret = 1;

  if (!list || !filename || !delim)
    return -1;

  if ((ret = strbuf_readfile(&filecontent, filename))) {
    /* empty or not readable */
  } else if ((entry = strtok(filecontent.buf, delim))) {
    clist_uniadd(list, entry);
    ret = 0;
    while ((entry = strtok(NULL, delim)))
      clist_uniadd(list, entry);
  }
  strbuf_free(&filecontent);
  return ret;
}

int isin_archive_domains(char *email)
{
  clistiter *iter;
  char *domain;

  if (!email || !config.archive_domains)
    return -1;

  /* check if its an emailadress and split domain part */
  if ((domain = rindex(email, '@')) && (domain - email) != strlen(email))
    domain++;
  else
    domain = email;

  for (iter = clist_begin(config.archive_domains); iter;
    iter = clist_next(iter)) {
    if (0 == strcasecmp(clist_content(iter), domain))
      return 1;
  }
  return 0;
}

int read_archive_domains()
{
  char *buf = NULL, *tmp;
  int buflen;

  if (openreadclose(ARCHIVE_DOMAINS_FILE, &buf, &buflen)) {
    debug("cant read archive domain file");

    return 1;
  }
  if (config.archive_domains) {
    clist_free_withcontent(config.archive_domains);
  }
  config.archive_domains = clist_new();

  if ((tmp = strtok(buf, "\n"))) {
    do {
      printf("adding %s\n", tmp);
      clist_uniadd(config.archive_domains, tmp);
    } while ((tmp = strtok(NULL, "\n")));
  }
  free(buf);
  return 0;
}

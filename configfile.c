#include "sp_fetchmail.h"
#include <libspcommon/minIni.h>
#include <libspcommon/openreadclose.h>

#include <string.h>
#include <stdlib.h>

#define sizearray(a)  (sizeof(a) / sizeof((a)[0]))

void use_configfile(char *configfile)
{
#define MAX_LINE_LEN 100
  char section[MAX_LINE_LEN - 2];
  char key[MAX_LINE_LEN / 2];
  char value[MAX_LINE_LEN / 2];
  int s, k;
  unsigned int loopcounter = 1;

  while (1) {
    if (!valid_license()) {
      syslog(LOG_ERR, "your license is expired. will refuse to do the job");
      sleep(2);
      errorout("your license is expired. will refuse to do the job");
    } else if (read_archive_domains()) {
      debug("no domains specified. nothing to do.");
      sleep(2);
      errorout("no domains specified. nothing to do.");
    } else {
      /* iterate over sections [accountname] */
      for (s = 0;
        ini_getsection(s, section, sizearray(section), configfile) > 0; s++) {
        /* iterate over keys */
        for (k = 0;
          ini_getkey(section, k, key, sizearray(key), configfile) > 0; k++) {
          ini_gets(section, key, NULL, value, sizearray(value), configfile);
          //printf("key=%s: value=%s\n", key, value);
          if (!memcmp(key, "user", 4)) {
            config.user = strdup(value);
          } else if (!memcmp(key, "password", 8)) {
            config.password = strdup(value);
          } else if (!memcmp(key, "tls", 3)) {
            config.connection_type = CONNECTION_TYPE_TLS;
          } else if (!memcmp(key, "disableenvelope", 15)) {
            config.disableenvelope = atoi(value);
          } else if (!memcmp(key, "keepmails", 9)) {
            config.keepmails = atoi(value);
          } else if (!memcmp(key, "maxsize", 7)) {
            config.maxsize = atoi(value);
          } else if (!memcmp(key, "port", 4)) {
            config.port = atoi(value);
          } else if (!memcmp(key, "recipheader", 11)) {
            config.recipheader = strdup(value);
          } else if (!memcmp(key, "server", 6)) {
            config.server = strdup(value);
            if (!memcmp(config.server, "127.0.0.1", 9))
              config.connection_type = CONNECTION_TYPE_PLAIN;
          } else if (!memcmp(key, "interval", 8)) {
            config.interval = atoi(value);
          } else if (!memcmp(key, "syncmode", 8)) {
            config.syncmode = atoi(value);
          } else if (!memcmp(key, "qmailstyle", 10)) {
            config.qmailstyle = atoi(value);
            if (0 > config.qmailstyle)
              config.qmailstyle = 1;
          } else if (!memcmp(key, "debug", 5)) {
            config.debug = atoi(value);
          } else if (!memcmp(key, "cachedir", 8)) {
            if (!config.cachedir) {
              config.cachedir = strdup(value);
            }
          } else if (!memcmp(key, "boxtype", 7)) {
            if (!memcmp(value, "POP3", 4)) {
              config.boxtype = POP3_STORAGE;
            } else if (!memcmp(value, "IMAP", 4)) {
              config.boxtype = IMAP_STORAGE;
            } else if (!memcmp(value, "AUTO", 4)) {
              config.boxtype = PROTO_AUTO;
            } else {
              config.boxtype = -1;
            }
          }
        }                       // key loop
        /* skip global section */
        if (!memcmp(section, "global", 6)) {
          continue;
        }

/* do this for all sections  in config file */
        if (config.syncmode) {
          syncbox();
        } else if (!config.user || !config.password || !config.server
          || 0 > config.boxtype || !config.interval) {
          syslog(LOG_ERR,
            "Error in configfile. please see example.conf. skipping %s",
            section);
        } else {
          /* get all on first run */
          if (!(loopcounter % config.interval) || (1 == loopcounter)) {
            syslog(LOG_INFO, "fetching mails from %s", section);
            fork_and_fetchit();
          }
          free_config();
        }
      }                         // section loop
      loopcounter++;
      if (UINT_MAX == loopcounter)
        loopcounter = 1;
      sleep(60);
    }
  }                             //end interval while loop
}

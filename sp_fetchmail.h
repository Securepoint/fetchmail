#ifndef SP_FETCHMAIL_H
#define SP_FETCHMAIL_H
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>

#include <libspcommon/sp_mailstorage.h>
#include <libspcommon/proctitle.h>

#define DEFAULT_CACHEDIR "/var/data/fetchmail"
#define DOVECOT_LMTP_SOCKET "/var/run/dovecot/lmtp"
#define ARCHIVE_DOMAINS_FILE "/etc/securepoint/qmail-filter/archive_domain"
#define EVILMAILSFILE "/var/data/fetchmail/evilmails"

#define debug(format,...) debug_real(__func__,format,## __VA_ARGS__)

struct mailstorage *source_storage;
struct mailstorage *target_storage;
struct mailsmtp *lmtp_storage;
clist *evilmails;

typedef enum
{
  syncmode_nothing = 0,
  syncmode_list_only,
  syncmode_selective,
  syncmode_all,
  syncmode_copyonly,
  syncmode_superheftig,
  syncmode_superheftig_list
} SyncMode;

typedef struct
{
  char *name;
  int count;
} Foldercount;

struct Config
{
  char *user;
  char *password;
  char *server;
  int boxtype;
  int port;
  int connection_type;
  int keepmails;
  int maxsize;
  int interval;
  char *pop3flagdir;
  char *tmpdir;
  /* global values (not freed) */
  char *cachedir;
  int qmailstyle;
  int debug;
  int disableenvelope;
  /* syncmode==1 just gives list of boxes in json on stdout */
  SyncMode syncmode;
  int verbose;
  struct mailimf_date_time *mailsbefore;
  struct mailimf_date_time *mailsafter;
  clist *archive_domains;
  char *recipheader;
} config;

typedef struct adressinfo
{
  clist *recipients;
  char *sender;
} addressinfo_t;

/* only used in syncmode UMA-specific */
struct TargetConfig
{
  char *server;
  char *user;
  char *boxlist;

} targetconfig;

void use_configfile(char *configfile);
void syncbox(void);
void usage(void);
void fork_and_fetchit(void);

/* helpers */
void cleanup(void);
int parse_timerange(const char *range);
int get_source_storage(void);
int get_target_storage(void);
int read_archive_domains(void);
int isin_archive_domains(char *email);
int init_msg_headerfields(mailmessage * msg);
/* appends folders of source_storage to list of struct Foldercount */
int append_folderlist(clist * folderlist);
int deliver_lmtp(char *data, size_t datalen, addressinfo_t * destusers,
  const char *folder);

int forkandwrite_qmailinject(char *data, size_t len);
int forkandwrite_dovecotlda(char *data, size_t datalen, const char *destuser,
  const char *folder);
int forkandwrite_extprogram(char *const argv[], char *data, size_t len);
void free_config(void);
void check_cachedir(void);
void debug_real(const char *func, const char *format, ...);
void errorout(const char *output);
void fetchmailsighandler(int sig);
int check_for_msjournal(struct mailmessage *msg);
int check_header(mailmessage * msg, addressinfo_t * addi);
/* clist helpers */
void addinfo_free(addressinfo_t * ai);
addressinfo_t *addinfo_new(void);
int clist_inlistremove(clist * list, char *entry);
void clist_uniadd(clist * list, char *entry);
int clist_free_withcontent(clist * list);
int clist_readfromfile(clist * list, const char *filename, const char *delim);
void clist_printout(clist * list, int fd, const char *delim);
#endif

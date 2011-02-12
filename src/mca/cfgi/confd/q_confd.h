/*
 * simplified API to confd
 */

#include "confd.h"
#include "confd_cdb.h"
#include "confd_maapi.h"

#ifdef NEED_BOOLEAN_DEFS

typedef int boolean;

#define TRUE  1
#define FALSE 0

#endif

#define N_QC_WRKSOCKS        1
#define N_QC_SUBPOINTS       4
#define N_QC_CMDPOINTS      32
#define N_QC_COMPPOINTS      8
#define N_QC_DATACBS         4

#define MAX_PATH_LEN     64

#define N_CMD_TAGS      4

typedef struct qc_confd qc_confd_t;

typedef struct cmdtbl cmdtbl_t;

/*
 * table mapping tags to a config command handler function
 */
struct cmdtbl {
    uint32_t  tags[N_CMD_TAGS];
    boolean (*handler)(confd_hkeypath_t         *kp,
                       enum cdb_iter_op          op,
                       confd_value_t            *value,
                       enum cdb_sub_notification notify_type,
                       long                      which);
    long      which;
    cmdtbl_t *left;
    cmdtbl_t *right;
};

/*
 * subscription point state
 */
struct subdata {
    enum cdb_sub_type         subtype;
    int                       priority;
    int                       sub2_flags;
    enum confd_iter_flags     iter_flags;
    int                       namespace;
    boolean                   qc_sub_flags;
    char                      path[MAX_PATH_LEN];
    cmdtbl_t                 *cmdtree;
    qc_confd_t               *cc;
    enum cdb_sub_notification notify_type;
    boolean                   prepare_failed;
    uint32_t                  prepare_fail_tag;
};

/*
 * CLI command point state
 */
struct cmddata {
    boolean    (*handler)(int                     maapisock,
                          struct confd_user_info *uinfo,
                          int                     argc,
                          char                   *argv[],
                          long                    which);
    long         which;
    char         cmdpoint[MAX_CALLPOINT_LEN];
};

/*
 * CLI completion point state
 */
struct compdata {
    boolean    (*handler)(struct confd_user_info *uinfo,
                          int                     cli_style,
                          char                   *token,
                          int                     comp_char,
                          confd_hkeypath_t       *kp,
                          char                   *cmdpath,
                          char                   *param_id);
    char         comppoint[MAX_CALLPOINT_LEN];
};

/*
 * data callback state block
 */
struct datacb {
    struct confd_data_cbs data_cbs;
    boolean               is_range;
    confd_value_t         lower, upper;
    char                  path[MAX_PATH_LEN];
};

/*
 * transaction callbacks - called once per-session (not once per-command)
 */
struct qc_trans_cbs {
    int         (*init) (qc_confd_t *, struct confd_trans_ctx *);
    int         (*finish) (qc_confd_t *, struct confd_trans_ctx *);
};

/*
 * confd connection state
 */
struct qc_confd {
    // address of confd daemon
    struct sockaddr_in       addr;

    // prefix and FILE for confd log messages
    char                     log_pfx[32];
    FILE                    *log_stream;
    enum confd_debug_level   log_level;

    // socket for cdb subscription notifications
    int                      subsock;

    // socket for other cdb interaction
    int                      cdbsock;

    // control socket for confd callback notification
    int                      ctlsock;
    // worker sockets for confd callback handling
    int                      wrksock[N_QC_WRKSOCKS];

    // for CLI interaction
    int                      maapisock;

    // subscription points
    int                      nsubpts;
    int                      subpts[N_QC_SUBPOINTS];
    struct subdata           subdata[N_QC_SUBPOINTS];

    // command points
    int                      ncmdpts;
    struct cmddata           cmddata[N_QC_CMDPOINTS];

    // completion points
    int                      ncomppts;
    struct compdata          compdata[N_QC_COMPPOINTS];

    // data callbacks
    int                      ndatacbs;
    struct datacb            datacb[N_QC_DATACBS];

    // thread calling cdb_trigger_subscriptions
    pthread_t                sub_trigger_id;

    // app-specific lock & unlock vectors
    void                   (*app_lock)(void);
    void                   (*app_unlock)(void);

    struct qc_trans_cbs      trans_cbs;

    struct cdb_txid          txid;
    struct confd_daemon_ctx *dctx;
    char                     err[128];
};

enum qc_subtype {
    QC_SUB_CONFIG        = CDB_SUB_RUNNING,
    QC_SUB_CONFIG_2PHASE = CDB_SUB_RUNNING_TWOPHASE,
    QC_SUB_OPER          = CDB_SUB_OPERATIONAL,
};

/*
 * qc_subscribe flags
 */
#define QC_SUB_ANCESTOR_DELETE  (1 <<  0)   // be notified about deleted parent
#define QC_SUB_EOD_NOTIFY       (1 <<  1)   // notify on end-of-diffs

/*
 * 'command' value that causes notifications about a given enum to be ignored
 */
#define        qc_cmd_ignore NULL

/*
 * enum value (outside the range of auto-generated hash values) that is used
 * to tag a function to be called after cdb_diff_iterate returns
 */
#define        qc_eod_notify ((1 << 31) + 2)


extern uint32_t qc_get_xmltag(confd_hkeypath_t *kp,
                              uint32_t          which);
extern confd_value_t *qc_find_key(confd_hkeypath_t *kp,
                                  uint32_t          xmltag,
                                  enum confd_vtype  type);
extern boolean qc_wait_start(qc_confd_t *cc);
extern void    qc_close(qc_confd_t *cc);
extern void qc_set_interfaces(char **interfaces);
extern int qc_get_interface(void);
extern boolean qc_confd_init(qc_confd_t           *cc,
                             char                  *log_pfx,
                             FILE                  *log_stream,
                             enum confd_debug_level log_level);
extern boolean qc_reconnect(qc_confd_t *cc);
extern boolean qc_startup_config_done(qc_confd_t *cc);
extern boolean qc_startup_config(qc_confd_t *cc);
extern boolean qc_subscribe(qc_confd_t     *cc,
                            enum qc_subtype subtype,
                            int             flags,      // set to 0 (for now)
                            int             priority,   // config only
                            int             namespace,
                            cmdtbl_t       *cmdtbl,
                            const char     *fmt, ...);
extern boolean qc_subscribe_done(qc_confd_t *cc);
extern boolean qc_handle_cb(qc_confd_t *cc,
                            int          sock);
extern void   *qc_confd_poll(void* arg);
extern void    qc_pp_change(char             *output,
                            int               output_size, 
                            confd_hkeypath_t *kp,
                            enum cdb_iter_op  op,
                            confd_value_t    *newv,
                            int               ns);
extern boolean qc_reg_cmdpoint(qc_confd_t  *cc,
                               char        *cmdpoint,
                               boolean    (*cmdfunc)
                                   (int                     maapisock,
                                    struct confd_user_info *uinfo,
                                    int                     argc,
                                    char                   *argv[],
                                    long                    which),
                               long         which);
extern boolean qc_reg_completion(qc_confd_t *cc,
                                 char       *comppoint,
                                 boolean   (*compfunc)
                                     (struct confd_user_info *uinfo,
                                      int                     cli_style,
                                      char                   *token,
                                      int                     comp_char,
                                      confd_hkeypath_t       *kp,
                                      char                   *cmdpath,
                                      char                   *param_id));
extern boolean qc_reg_data_cb_range(qc_confd_t            *cc,
                                    struct confd_data_cbs *dcp,
                                    confd_value_t         *lower,
                                    confd_value_t         *upper,
                                    const char            *fmt, ...);
extern boolean qc_register_trans_cb(qc_confd_t *cc, struct qc_trans_cbs *);
extern boolean qc_callbacks_done(qc_confd_t *cc);
extern int     qc_open_cdb(qc_confd_t     *cc,
                           enum cdb_db_type db,
                           int              flags);
extern void    qc_close_cdb(int cdbsock);

static inline boolean
qc_reg_data_cb (qc_confd_t            *cc,
                struct confd_data_cbs *dcp)
{
    return qc_reg_data_cb_range(cc, dcp, NULL, NULL, NULL);
}

/*
 * return TRUE if the element is being deleted
 */
static inline boolean
qc_nocmd (enum cdb_iter_op op)
{
    if (op == MOP_DELETED) {
        return TRUE;
    }

    return FALSE;
}

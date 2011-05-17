/*
 * simplified API to confd that handles error reporting and
 * reconnecting to confd after a restart
 *
 * app should startup with:
 *   qc_confd_init(&cc, ...)
 *   qc_wait_start
 *   qc_subscribe
 *   qc_subscribe_done          // if any subscriptions
 *   qc_reg_cmdpoint
 *   qc_reg_data_cb
 *   qc_callbacks_done          // if any cmd or data callbacks
 *   qc_startup_config
 *   qc_confd_poll
 *
 * app can reconnect to confd with:
 *   qc_close
 *   qc_reconnect
 *   qc_confd_poll
 */
#include "openrcm_config_private.h"
#include "constants.h"

#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <poll.h>
#include <errno.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "opal/util/output.h"
#include "mca/cfgi/base/public.h"

// from types.h
#define NUM_OF(x) (sizeof(x)/sizeof(x[0]))

/*
 * avoid warnings when converting between pointers and
 * differently-sized integers
 */
#define int2ptr(i)      ((void*)(size_t)(i))
#define ptr2int(p)      ((size_t)(p))

typedef int boolean;

#define TRUE  1
#define FALSE 0
// end types.h

static bool qc_debug = false;

#include "orcm_q_confd.h"

 /*
 * use CONFD_ADDR (format ip-address:port), or
 * default to 127.0.0.1:CONFD_PORT
 */
static void
cc_addr_init (struct sockaddr_in *sin)
{
    char *ap, *cp;
    ushort confd_port;
    in_addr_t s_addr;

    confd_port = CONFD_PORT;
    if ((ap = getenv("CONFD_ADDR"))) {
        if ((cp = index(ap, ':'))) {
            *cp++ = '\0';
            confd_port = strtoul(cp, NULL, 0);
        }
        s_addr = inet_addr(ap);
        if (s_addr == -1) {
            s_addr = inet_addr("127.0.0.1");
        }
    } else {
        s_addr = inet_addr("127.0.0.1");
    }

    sin->sin_family      = AF_INET;
    sin->sin_addr.s_addr = s_addr;
    sin->sin_port        = htons(confd_port);
}


/*
 * retrieve confd last error string (w/sanity checks)
 */
static char*
confd_last_err (void)
{
    char *lasterr;

    lasterr = confd_lasterr();
    if (lasterr && *lasterr) {
        return lasterr;
    }
    return "unknown err";
}


/*
 * write an error string into the error buffer
 */
static void
cc_err (qc_confd_t *cc,
        const char *fmt, ...)
{
    va_list args;

    if (cc->err[0] == '\0') {
        /*
         * don't overwrite a previously reported error
         */
        va_start(args, fmt);
        vsnprintf(cc->err, sizeof(cc->err), fmt, args);
        va_end(args);

    }

    /*
     * always output the error message on the log_stream
     */
    if (cc->log_stream) {
        va_start(args, fmt);
        fprintf(cc->log_stream, "%s: ", cc->log_pfx);
        vfprintf(cc->log_stream, fmt, args);
        fprintf(cc->log_stream, "\n");
        va_end(args);
    }
}

/*****************************************************************************/

/*
 * find and return the 'which' (1st, 2nd, etc.) XML tag in the keypath
 */
uint32_t
qc_get_xmltag (confd_hkeypath_t *kp,
               uint32_t          which)
{
    int ix;
    confd_value_t *vp;

    for (ix = 0; ix < kp->len; ++ix) {
        vp = &kp->v[ix][0];
        if (vp->type == C_XMLTAG &&
            --which  == 0) {
            return vp->val.xmltag.tag;
        }
    }

    return 0;
}


/*
 * return the key for the element with the given XML tag
 * if type is non-zero, the key must have the specified type
 */
confd_value_t *
qc_find_key (confd_hkeypath_t *kp,
             uint32_t          xmltag,
             enum confd_vtype  type)
{
    int depth;
    confd_value_t *vp;

    /*
     * scan the path components for the specified tag
     */
    for (depth = 0; depth < kp->len; ++depth) {
	vp = &kp->v[depth][0];

	if (vp->type == C_XMLTAG &&
	    vp->val.xmltag.tag == xmltag) {
            if (depth == 0) {
                break;                  /* no key present */
            }

            /*
             * point to where the key would be
             */
	    vp = &kp->v[depth - 1][0];

            /*
             * check the type doesn't matter, or that it matches
             */
            if (type == 0 ||
                type == vp->type) {
                return vp;
            }
            break;
	}
    }

    return NULL;
}


/*
 * wait for confd
 */
boolean
qc_wait_start (qc_confd_t *cc)
{
    if (cdb_wait_start(cc->cdbsock) != CONFD_OK) {
        cc_err(cc, "wait_start: %s", confd_last_err());
        return FALSE;
    }

    return TRUE;
}


/*
 * pretty-print a config change
 */
void
qc_pp_change (char             *output,
              int               output_size, 
              confd_hkeypath_t *kp,
              enum cdb_iter_op  op,
              confd_value_t    *newv,
              int               ns)
{
    char *opstr;
    char buf[256], value[256];

    switch (op) {
      case MOP_CREATED:   opstr = "create";    break;
      case MOP_DELETED:   opstr = "delete";    break;
      case MOP_MODIFIED:  opstr = "modify";    break;
      case MOP_VALUE_SET: opstr = "set   ";    break;
      default:            opstr = "<UNKNOWN>"; break;
    }

    confd_pp_kpath(buf, sizeof(buf), kp);
    if (op == MOP_VALUE_SET) {
	confd_ns_pp_value(value, sizeof(value), newv, ns);
	snprintf(output, output_size, "%s %s = %s",opstr, buf, value);
    } else {
        snprintf(output, output_size, "%s %s", opstr, buf);
    }
}


/*
 * close sockets to confd
 */
void
qc_close (qc_confd_t *cc)
{
    int ix;

    if (cc->ctlsock >= 0) {
        close(cc->ctlsock);
        cc->ctlsock = -1;
    }

    for (ix = 0; ix < NUM_OF(cc->wrksock); ++ix) {
        if (cc->wrksock[ix] >= 0) {
            close(cc->wrksock[ix]);
            cc->wrksock[ix] = -1;
        }
    }

    if (cc->subsock >= 0) {
        close(cc->subsock);
        cc->subsock = -1;
    }

    if (cc->cdbsock >= 0) {
        close(cc->cdbsock);
        cc->cdbsock = -1;
    }

    if (cc->maapisock >= 0) {
        close(cc->maapisock);
        cc->maapisock = -1;
    }

    if (cc->dctx) {
        confd_release_daemon(cc->dctx);
        cc->dctx = NULL;
    }
}


/*
 * open a data socket connected to CDB
 */
static boolean
cc_cdbsock (qc_confd_t  *cc,
            int         *sockptr)
{
    int cdbsock;

    cdbsock = socket(PF_INET, SOCK_STREAM, 0);
    if (cdbsock < 0) {
        cc_err(cc, "socket(cdbsock): %s", strerror(errno));
        return FALSE;
    }

    if (cdb_connect(cdbsock, CDB_DATA_SOCKET, (struct sockaddr*) &cc->addr,
                    sizeof(cc->addr)) != CONFD_OK) {
        if (qc_debug) {
            cc_err(cc, "cdb_connect(%s:%u): %s",
                   inet_ntoa(cc->addr.sin_addr), ntohs(cc->addr.sin_port),
                   confd_last_err());
        }
        close(cdbsock);
        return FALSE;
    }

    *sockptr = cdbsock;

    return TRUE;
}


/*
 * connect to confd
 */
static boolean
cc_connect (qc_confd_t *cc)
{
    /*
     * so confd_pp_kpath() can print text path component names
     * (though enum values aren't printed by confd_ns_pp_value)
     */
    confd_load_schemas((struct sockaddr*) &cc->addr, sizeof(cc->addr));

    cc->dctx = confd_init_daemon(cc->log_pfx);
    if (cc->dctx == NULL) {
        cc_err(cc, "confd_init_daemon: %s", confd_last_err());
        return FALSE;
    }
    cc->dctx->d_opaque = cc;

    if (! cc_cdbsock(cc, &cc->cdbsock)) {
        return FALSE;
    }

    return TRUE;
}


/*
 * initialize our confd context
 */
static void
cc_init (qc_confd_t *cc)
{
    int ix;

    /*
     * initialize the context to invalid values
     */
    bzero(cc, sizeof(*cc));
    cc->ctlsock = -1;
    for (ix = 0; ix < NUM_OF(cc->wrksock); ++ix) {
        cc->wrksock[ix] = -1;
    }
    cc->subsock   = -1;
    cc->cdbsock   = -1;
    cc->maapisock = -1;
}

/*
 * initialize confd & cdb connections and state
 */
boolean
qc_confd_init (qc_confd_t            *cc,
               char                  *log_pfx,
               FILE                  *log_stream,
               enum confd_debug_level log_level)
{
    /* simplify the debug flag */
    if (4 < opal_output_get_verbosity(orcm_cfgi_base.output)) {
        qc_debug = true;
    } else {
        qc_debug = false;
    }

    cc_init(cc);

    cc_addr_init(&cc->addr);

    confd_init(log_pfx, log_stream, log_level);

    snprintf(cc->log_pfx, sizeof(cc->log_pfx), "%s", log_pfx);
    cc->log_stream = log_stream;
    if (log_stream) {
        cc->log_level = log_level;
    } else {
        cc->log_level = CONFD_SILENT;
    }

    return cc_connect(cc);
}


/*
 * blocking thread to trigger "config change" notifications for everything
 * in the startup config
 *
 * subscriptions need to be registered before calling this
 */
static void*
startup_config_thread (void* arg)
{
    int ix, cfg_ix;
    qc_confd_t *cc;
    int cfg_subpts[NUM_OF(cc->subpts)];

    cc = arg;

    /*
     * create a subpt array of only config subscription points
     */
    for (ix = cfg_ix = 0; ix < cc->nsubpts; ++ix) {
        if (cc->subdata[ix].subtype == CDB_SUB_OPERATIONAL) {
            continue;
        }
        cfg_subpts[cfg_ix++] = cc->subpts[ix];
    }


    if (cdb_trigger_subscriptions(cc->cdbsock,
                                  cfg_subpts, cfg_ix) != CONFD_OK) {
        cc_err(cc, "cdb_trigger_sub: %s", confd_last_err());
        return int2ptr(CONFD_ERR);
    }

    return int2ptr(CONFD_OK);
}


/*
 * blocks until the startup config for the existing subscription points
 * has been processed
 */
boolean
qc_startup_config_done (qc_confd_t *cc)
{
    void *retval;

    if (pthread_join(cc->sub_trigger_id, &retval) != 0) {
        cc_err(cc, "pthread_join(startup_config): %s", strerror(errno));
        return FALSE;
    }

    if (ptr2int(retval) != CONFD_OK) {
        /*
         * the thread generated an error
         */ 
        return FALSE;
    }

    return TRUE;
}


/*
 * create a thread to trigger processing the startup config
 */
boolean
qc_startup_config (qc_confd_t *cc)
{
    if (cc->nsubpts == 0) {
        return TRUE;
    }

    if (pthread_create(&cc->sub_trigger_id,
		       NULL,                    /* thread attributes */
		       startup_config_thread,
		       cc                       /* thread parameter */) < 0) {
	cc_err(cc, "pthread_create(startup_config): %s", strerror(errno));
        return FALSE;
    }

    return TRUE;
}


/*
 * open control & worker sockets
 */
static boolean
cc_open_cbsocks (qc_confd_t *cc)
{
    int ix;

    /*
     * check if this hasn't been done yet
     */
    if (cc->ctlsock < 0) {
        cc->ctlsock = socket(PF_INET, SOCK_STREAM, 0);
        if (cc->ctlsock < 0) {
            cc_err(cc, "socket(control): %s", strerror(errno));
            return FALSE;
        }

        if (confd_connect(cc->dctx, cc->ctlsock, CONTROL_SOCKET, 
                          (struct sockaddr*) &cc->addr,
                          sizeof(cc->addr)) < 0) {
            cc_err(cc, "confd_connect(ctl, %s:%u): %s",
                   inet_ntoa(cc->addr.sin_addr), ntohs(cc->addr.sin_port),
                   confd_last_err());
            close(cc->ctlsock);
            cc->ctlsock = -1;
            return FALSE;
        }
    }

    for (ix = 0; ix < N_QC_WRKSOCKS; ++ix) {
        if (cc->wrksock[ix] < 0) {
            cc->wrksock[ix] = socket(PF_INET, SOCK_STREAM, 0);
            if (cc->wrksock[ix] < 0) {
                cc_err(cc, "socket(worker): %s", strerror(errno));
                return FALSE;
            }

            if (confd_connect(cc->dctx, cc->wrksock[ix], WORKER_SOCKET,
                              (struct sockaddr*) &cc->addr,
                              sizeof(cc->addr)) < 0) {
                cc_err(cc, "confd_connect(wrk, %s:%u): %s",
                       inet_ntoa(cc->addr.sin_addr), ntohs(cc->addr.sin_port),
                       confd_last_err());
                close(cc->wrksock[ix]);
                cc->wrksock[ix] = -1;
                return FALSE;
            }
        }
    }

    return TRUE;
}


/*
 * register a subscription with confd
 */
static boolean
cc_reg_subpt (qc_confd_t     *cc,
              struct subdata *sdp,
              int            *subpt)
{
    /*
     * open ctl/wrk sockets
     */
    if (! cc_open_cbsocks(cc)) {
        return FALSE;
    }

    /*
     * open the subscription socket if this is the first subscription
     */
    if (cc->subsock < 0) {
        cc->subsock = socket(PF_INET, SOCK_STREAM, 0);
        if (cc->subsock < 0) {
            cc_err(cc, "socket(subsock): %s", strerror(errno));
            return FALSE;
        }

        if (cdb_connect(cc->subsock, CDB_SUBSCRIPTION_SOCKET,
                        (struct sockaddr*) &cc->addr, sizeof(cc->addr)) < 0) {
            cc_err(cc, "cdb_connect(sub, %s:%u): %s",
                   inet_ntoa(cc->addr.sin_addr), ntohs(cc->addr.sin_port),
                   confd_last_err());
            close(cc->subsock);
            cc->subsock = -1;
            return FALSE;
        }
    }

    /*
     * bug in confd (fixed in 3.1.2)
     * subscribe2 doesn't work with operational data
     */
    if (((sdp->subtype == CDB_SUB_OPERATIONAL)
                ?
         cdb_oper_subscribe(cc->subsock,
                            sdp->namespace,
                            subpt,
                            sdp->path)
                :
         cdb_subscribe2(cc->subsock,
                        sdp->subtype,
                        sdp->sub2_flags,
                        sdp->priority,
                        subpt,
                        sdp->namespace,
                        sdp->path))
                != CONFD_OK) {
        cc_err(cc, "cdb_subscribe2(%s): %s", sdp->path, confd_last_err());
        return FALSE;
    }

    return TRUE;
}


/*
 * allocate a worker socket to an action
 */
static int
cc_action_init (struct confd_user_info *uinfo)
{
    qc_confd_t *cc;

    cc = uinfo->actx.dx->d_opaque;

    confd_action_set_fd(uinfo, cc->wrksock[0]);

    return CONFD_OK;
}


/*
 * invoke a command handler
 */
static int
cc_command (struct confd_user_info *uinfo,
            char                   *path,
            int                     argc,
            char                  **argv)
{
    int retv;
    qc_confd_t *cc;
    struct cmddata *cdp;

    cdp = uinfo->actx.cb_opaque;
    cc  = uinfo->actx.dx->d_opaque;

    if (cc->maapisock < 0) {
        cc->maapisock = socket(PF_INET, SOCK_STREAM, 0);
        if (cc->maapisock < 0 ) {
            cc_err(cc, "socket(maapi): %s", strerror(errno));
            confd_action_seterr(uinfo, "%s", cc->err);
            return CONFD_ERR;
        }
    
        if (maapi_connect(cc->maapisock, (struct sockaddr*) &cc->addr,
                          sizeof(cc->addr)) < 0) {
            cc_err(cc, "maapi_connect(%s:%u): %s",
                   inet_ntoa(cc->addr.sin_addr), ntohs(cc->addr.sin_port),
                   confd_last_err());
            confd_action_seterr(uinfo, "%s", cc->err);

            close(cc->maapisock);
            cc->maapisock = -1;

            return CONFD_ERR;
        }
    }

    /*
     * for all cmdpoint callbacks, argv[0] contains the command name or path
     * which is not useful
     */
    argc--;
    argv++;

    /*
     * for <show> callbacks, argv[] contains
     * - arguments specified with <args>
     * - a "0"
     * - path to data element (e.g. "/ns:path/to/elem")
     * - empty string
     *
     * remove the final empty string
     */
    if (argv[argc - 1][0] == '\0') {
        --argc;
    }

    if (cc->app_lock) {
        cc->app_lock();
    }
    if (cdp->handler(cc->maapisock, uinfo, argc, argv, cdp->which)) {
        retv = CONFD_OK;
    } else {
        confd_action_seterr(uinfo, "CLI cmd fail: %s", cdp->cmdpoint);
        retv = CONFD_ERR;
    }
    if (cc->app_unlock) {
        cc->app_unlock();
    }

    return retv;
}


/*
 * (re)register a command point handler
 */
static boolean
cc_reg_cmdpt (qc_confd_t     *cc,
              struct cmddata *cdp)
{
    struct confd_action_cbs act_cbs;

    /*
     * open ctl/wrk sockets
     */
    if (! cc_open_cbsocks(cc)) {
        return FALSE;
    }

    snprintf(act_cbs.actionpoint, sizeof(act_cbs.actionpoint),
             "%s", cdp->cmdpoint);

    act_cbs.init      = cc_action_init;
    act_cbs.command   = cc_command;
    act_cbs.cb_opaque = cdp;

    if (confd_register_action_cbs(cc->dctx, &act_cbs) != CONFD_OK) {
        cc_err(cc, "reg_cmdpt(%s): %s", cdp->cmdpoint, confd_last_err());
        return FALSE;
    }

    return TRUE;
}


/*
 * invoke a completion handler
 */
static int
cc_completion (struct confd_user_info *uinfo,
               int                     cli_style,
               char                   *token,
               int                     completion_char,
               confd_hkeypath_t       *kp,
               char                   *cmdpath,
               char                   *cmdparam_id,
               struct confd_qname     *simpleType,
               char                   *extra)
{
    int retv;
    qc_confd_t *cc;
    struct compdata *cmp;
    char buf[512];

    cmp = uinfo->actx.cb_opaque;
    cc  = uinfo->actx.dx->d_opaque;

    if (cc->log_level == CONFD_TRACE) {
        fprintf(cc->log_stream, "completion(%s): %s\n", cmp->comppoint, cmdpath);
        if (NULL != kp && 0 < kp->len) {
            confd_pp_kpath(buf, sizeof(buf), kp);
            fprintf(cc->log_stream, "        kp: %s\n", buf);
        }
    }
    
    if (cc->app_lock) {
        cc->app_lock();
    }
    if (cmp->handler(uinfo,
                     cli_style,
                     token,
                     completion_char,
                     kp,
                     cmdpath,
                     cmdparam_id)) {
        retv = CONFD_OK;
    } else {
        confd_action_seterr(uinfo, "completion failure: %s", cmp->comppoint);
        retv = CONFD_ERR;
    }
    if (cc->app_unlock) {
        cc->app_unlock();
    }

    return retv;
}


/*
 * (re)register a completion point handler
 */
static boolean
cc_reg_comppt (qc_confd_t      *cc,
               struct compdata *cmp)
{
    struct confd_action_cbs act_cbs;

    /*
     * open ctl/wrk sockets
     */
    if (! cc_open_cbsocks(cc)) {
        return FALSE;
    }

    snprintf(act_cbs.actionpoint, sizeof(act_cbs.actionpoint),
             "%s", cmp->comppoint);

    act_cbs.init       = cc_action_init;
    act_cbs.completion = cc_completion;
    act_cbs.cb_opaque  = cmp;

    if (confd_register_action_cbs(cc->dctx, &act_cbs) != CONFD_OK) {
        cc_err(cc, "reg_completion(%s): %s", cmp->comppoint, confd_last_err());
        return FALSE;
    }

    return TRUE;
}


/*
 * allocate a worker socket for a transaction
 */
static int
cc_trans_init (struct confd_trans_ctx *tctx)
{
    qc_confd_t *cc;
    int         retv;

    cc = tctx->dx->d_opaque;
    retv = CONFD_OK;

    if (cc->trans_cbs.init) {
        retv = cc->trans_cbs.init(cc, tctx);
    }

    confd_trans_set_fd(tctx, cc->wrksock[0]);

    return retv;
}

/*
 * cleanup after a transaction
 */
static int
cc_trans_finish (struct confd_trans_ctx *tctx)
{
    qc_confd_t *cc;

    cc = tctx->dx->d_opaque;
    if (cc->trans_cbs.finish) {
        return(cc->trans_cbs.finish(cc, tctx));
    }

    return CONFD_OK;
}


/*
 * set up the transaction callback handlers
 */
static boolean
cc_reg_trans (qc_confd_t *cc)
{
    struct confd_trans_cbs trans_cbs = {
        .init   = cc_trans_init,
        .finish = cc_trans_finish,
    };

    if (confd_register_trans_cb(cc->dctx, &trans_cbs) != CONFD_OK) {
        cc_err(cc, "reg_trans_cb: %s", confd_last_err());
        return FALSE;
    }

    return TRUE;
}


/*
 * data callback wrapper functions
 */
static int
wrap_cb_exists_optional (struct confd_trans_ctx *tctx,
		         confd_hkeypath_t       *kp)
{
    int retv;
    struct confd_data_cbs *dcbs = tctx->cb_opaque;
    qc_confd_t            *cc   = tctx->dx->d_opaque;

    cc->app_lock();
    retv = dcbs->exists_optional(tctx, kp);
    cc->app_unlock();

    return retv;
}

static int
wrap_cb_get_elem (struct confd_trans_ctx *tctx,
                  confd_hkeypath_t       *kp)
{
    int retv;
    struct confd_data_cbs *dcbs = tctx->cb_opaque;
    qc_confd_t            *cc   = tctx->dx->d_opaque;

    cc->app_lock();
    retv = dcbs->get_elem(tctx, kp);
    cc->app_unlock();

    return retv;
}

static int
wrap_cb_get_next (struct confd_trans_ctx *tctx,
                  confd_hkeypath_t       *kp,
                  long                    next)
{
    int retv;
    struct confd_data_cbs *dcbs = tctx->cb_opaque;
    qc_confd_t            *cc   = tctx->dx->d_opaque;

    cc->app_lock();
    retv = dcbs->get_next(tctx, kp, next);
    cc->app_unlock();

    return retv;
}

static int
wrap_cb_set_elem (struct confd_trans_ctx *tctx,
                  confd_hkeypath_t       *kp,
                  confd_value_t          *newval)
{
    int retv;
    struct confd_data_cbs *dcbs = tctx->cb_opaque;
    qc_confd_t            *cc   = tctx->dx->d_opaque;

    cc->app_lock();
    retv = dcbs->set_elem(tctx, kp, newval);
    cc->app_unlock();

    return retv;
}

static int
wrap_cb_create (struct confd_trans_ctx *tctx,
                confd_hkeypath_t       *kp)
{
    int retv;
    struct confd_data_cbs *dcbs = tctx->cb_opaque;
    qc_confd_t            *cc   = tctx->dx->d_opaque;

    cc->app_lock();
    retv = dcbs->create(tctx, kp);
    cc->app_unlock();

    return retv;
}

static int
wrap_cb_remove (struct confd_trans_ctx *tctx,
                confd_hkeypath_t       *kp)
{
    int retv;
    struct confd_data_cbs *dcbs = tctx->cb_opaque;
    qc_confd_t            *cc   = tctx->dx->d_opaque;

    cc->app_lock();
    retv = dcbs->remove(tctx, kp);
    cc->app_unlock();

    return retv;
}

static int
wrap_cb_num_instances (struct confd_trans_ctx *tctx,
                       confd_hkeypath_t       *kp)
{
    int retv;
    struct confd_data_cbs *dcbs = tctx->cb_opaque;
    qc_confd_t            *cc   = tctx->dx->d_opaque;

    cc->app_lock();
    retv = dcbs->num_instances(tctx, kp);
    cc->app_unlock();

    return retv;
}

static int
wrap_cb_get_object (struct confd_trans_ctx *tctx,
                    confd_hkeypath_t       *kp)
{
    int retv;
    struct confd_data_cbs *dcbs = tctx->cb_opaque;
    qc_confd_t            *cc   = tctx->dx->d_opaque;

    cc->app_lock();
    retv = dcbs->get_object(tctx, kp);
    cc->app_unlock();

    return retv;
}

static int
wrap_cb_get_next_object (struct confd_trans_ctx *tctx,
                         confd_hkeypath_t       *kp,
                         long                    next)
{
    int retv;
    struct confd_data_cbs *dcbs = tctx->cb_opaque;
    qc_confd_t            *cc   = tctx->dx->d_opaque;

    cc->app_lock();
    retv = dcbs->get_next_object(tctx, kp, next);
    cc->app_unlock();

    return retv;
}

static int
wrap_cb_get_case (struct confd_trans_ctx *tctx,
                  confd_hkeypath_t       *kp,
                  confd_value_t          *choice)
{
    int retv;
    struct confd_data_cbs *dcbs = tctx->cb_opaque;
    qc_confd_t            *cc   = tctx->dx->d_opaque;

    cc->app_lock();
    retv = dcbs->get_case(tctx, kp, choice);
    cc->app_unlock();

    return retv;
}

static int
wrap_cb_set_case (struct confd_trans_ctx *tctx,
                  confd_hkeypath_t       *kp,
                  confd_value_t          *choice,
                  confd_value_t          *caseval)
{
    int retv;
    struct confd_data_cbs *dcbs = tctx->cb_opaque;
    qc_confd_t            *cc   = tctx->dx->d_opaque;

    cc->app_lock();
    retv = dcbs->set_case(tctx, kp, choice, caseval);
    cc->app_unlock();

    return retv;
}

static int
wrap_cb_get_attrs (struct confd_trans_ctx *tctx,
                   confd_hkeypath_t       *kp,
                   u_int32_t              *attrs,
                   int                     num_attrs)
{
    int retv;
    struct confd_data_cbs *dcbs = tctx->cb_opaque;
    qc_confd_t            *cc   = tctx->dx->d_opaque;

    cc->app_lock();
    retv = dcbs->get_attrs(tctx, kp, attrs, num_attrs);
    cc->app_unlock();

    return retv;
}

static int
wrap_cb_set_attr (struct confd_trans_ctx *tctx,
                  confd_hkeypath_t       *kp,
                  u_int32_t               attr,
                  confd_value_t          *v)
{
    int retv;
    struct confd_data_cbs *dcbs = tctx->cb_opaque;
    qc_confd_t            *cc   = tctx->dx->d_opaque;

    cc->app_lock();
    retv = dcbs->set_attr(tctx, kp, attr, v);
    cc->app_unlock();

    return retv;
}


/*
 * register a data callback
 */
static boolean
cc_reg_datacb (qc_confd_t     *cc,
               struct datacb *dcp)
{
    struct confd_data_cbs *dcbs, wrap_cbs = { };

    dcbs = &dcp->data_cbs;

    /*
     * if lock/unlock functions are defined, substitute wrapper functions
     * that wrap the user vectors with lock & unlock calls
     */
    if (cc->app_lock && cc->app_unlock) {
        strcpy(wrap_cbs.callpoint, dcbs->callpoint);

#define CB_SET(w, a, e) w.e = a->e ? wrap_cb_ ## e : NULL

        CB_SET(wrap_cbs, dcbs, exists_optional);
        CB_SET(wrap_cbs, dcbs, get_elem);
        CB_SET(wrap_cbs, dcbs, get_next);
        CB_SET(wrap_cbs, dcbs, set_elem);
        CB_SET(wrap_cbs, dcbs, create);
        CB_SET(wrap_cbs, dcbs, remove);
        CB_SET(wrap_cbs, dcbs, num_instances);
        CB_SET(wrap_cbs, dcbs, get_object);
        CB_SET(wrap_cbs, dcbs, get_next_object);
        CB_SET(wrap_cbs, dcbs, get_case);
        CB_SET(wrap_cbs, dcbs, set_case);
        CB_SET(wrap_cbs, dcbs, get_attrs);
        CB_SET(wrap_cbs, dcbs, set_attr);

#undef CB_SET

        wrap_cbs.cb_opaque = dcbs;

        dcbs = &wrap_cbs;
    }

    if (dcp->is_range) {
        if (confd_register_range_data_cb(cc->dctx, dcbs,
                                         &dcp->lower, &dcp->upper, 1,
                                         dcp->path) != CONFD_OK) {
            cc_err(cc, "reg_data_cb(%s, %s): %s",
                   dcp->data_cbs.callpoint, dcp->path, confd_lasterr());
            return FALSE;
        }
    } else {
        if (confd_register_data_cb(cc->dctx, dcbs) != CONFD_OK) {
            cc_err(cc, "reg_data_cb(%s): %s",
                   dcp->data_cbs.callpoint, confd_lasterr());
            return FALSE;
        }
    }

    return TRUE;
}


/*
 * register (and remember) a data callback with optional range constraint
 */
boolean
qc_reg_data_cb_range (qc_confd_t            *cc,
                      struct confd_data_cbs *dcbs,
                      confd_value_t         *lower,
                      confd_value_t         *upper,
                      const char            *fmt, ...)
{
    struct datacb *dcp;

    /*
     * open ctl/wrk sockets
     */
    if (! cc_open_cbsocks(cc)) {
        return FALSE;
    }

    if (cc->ndatacbs >= N_QC_DATACBS) {
        cc_err(cc, "too many data callbacks");
        return FALSE;
    }

    if (cc->ndatacbs == 0) {
        if (! cc_reg_trans(cc)) {
            return FALSE;
        }
    }

    dcp = &cc->datacb[cc->ndatacbs];
    if (fmt && lower && upper) {
        char *path;
        va_list args;

        path = dcp->path;
        va_start(args, fmt);
        if (vsnprintf(path, MAX_PATH_LEN, fmt, args) >= MAX_PATH_LEN) {
            va_end(args);
            cc_err(cc, "data callback path too long: %s", path);
            return FALSE;
        }
        va_end(args);
        dcp->lower    = *lower;
        dcp->upper    = *upper;
        dcp->is_range = TRUE;
    } else {
        dcp->is_range = FALSE;
    }

    dcp->data_cbs = *dcbs;
    if (! cc_reg_datacb(cc, dcp)) {
        return FALSE;
    }

    cc->ndatacbs++;

    return TRUE;
}


/*
 * reconnect after a close
 */
boolean
qc_reconnect (qc_confd_t *cc)
{
    int ix;

    if (! cc_connect(cc)) {
        return FALSE;
    }

    qc_wait_start(cc);

    // reregister subscriptions
    for (ix = 0; ix < cc->nsubpts; ++ix) {
        if (! cc_reg_subpt(cc, &cc->subdata[ix], &cc->subpts[ix])) {
            return FALSE;
        }
    }

    // reregister command callbacks
    for (ix = 0; ix < cc->ncmdpts; ++ix) {
        if (! cc_reg_cmdpt(cc, &cc->cmddata[ix])) {
            return FALSE;
        }
    }

    // reregister completion callbacks
    for (ix = 0; ix < cc->ncomppts; ++ix) {
        if (! cc_reg_comppt(cc, &cc->compdata[ix])) {
            return FALSE;
        }
    }

    // reregister data callback handlers
    if (cc->ndatacbs) {
        if (! cc_reg_trans(cc)) {
            return FALSE;
        }
        for (ix = 0; ix < cc->ndatacbs; ++ix) {
            if (! cc_reg_datacb(cc, &cc->datacb[ix])) {
                return FALSE;
            }
        }
    }

    if (cc->ncmdpts || cc->ncomppts || cc->ndatacbs) {
        if (! qc_callbacks_done(cc)) {
            return FALSE;
        }
    }

    if (cc->nsubpts > 0) {
        if (! qc_subscribe_done(cc)) {
            return FALSE;
        }
    }

    /// ask confd to tell us what changed in the config since we restarted

    return TRUE;
}


/*
 * create a binary tree with the specified "depth" from a sorted cmdtbl
 * list (pointed to by "*hp"), leaving "*empty" leaf nodes unpopulated
 */
static cmdtbl_t*
tbltree (cmdtbl_t **hp,
         uint32_t   depth,
         uint32_t  *empty)
{
    cmdtbl_t *tp, *left;

    // check for the list being empty
    if (*hp == NULL) {
	return NULL;
    }

    // if there's only one node to populate
    if (depth == 1) {
	// consume an unpopulated node if possible
	if (*empty) {
	    (*empty)--;
	    return NULL;
	}

	// dequeue a node from the list and return it
	tp  = *hp;
	*hp = tp->right;
	tp->right = NULL;

	return tp;
    }

    // build the tree to the left of our root node
    left = tbltree(hp, depth - 1, empty);

    // dequeue our root node
    tp  = *hp;
    *hp = tp->right;
    tp->right = NULL;

    // attach the left and right subtrees to it
    tp->left  = left;
    tp->right = tbltree(hp, depth - 1, empty);

    return tp;
}


/*
 * compare the tag arrays in element 'e' to those in element 'l'
 */
static enum {
    TAGS_EQ,
    TAGS_LT,
    TAGS_GT,
} tag_cmp (cmdtbl_t *e,
           cmdtbl_t *l)
{
    int ix;

    for (ix = 0; ix < N_CMD_TAGS; ++ix) {
	if (e->tags[ix] < l->tags[ix]) {
	    return TAGS_LT;
	}
	if (e->tags[ix] > l->tags[ix]) {
	    return TAGS_GT;
	}
    }

    return TAGS_EQ;
}


/*
 * thread the elements in the cmdtbl array into an ordered binary tree
 * and return the root of the tree
 */
static cmdtbl_t*
cmdtbl_thread (qc_confd_t *cc,
               cmdtbl_t   *cmdtbl)
{
    uint32_t cmds, depth, empty, cmdbits;
    cmdtbl_t *head, **pp;

    // an empty array
    if (cmdtbl->tags[0] == 0) {
        cc_err(cc, "empty cmd table");
	return NULL;
    }

    /*
     * insertion sort by tags[]
     */
    head = cmdtbl++;
    pp   = &head;
    cmds = 1;
    while (cmdtbl->tags[0]) {
        int fx, bx;
        uint32_t tmp;

        // find the last tag
        for (bx = N_CMD_TAGS - 1; bx > 0; --bx) {
            if (cmdtbl->tags[bx])
                break;
        }
        // reverse the order of tags[]
        for (fx = 0; fx < bx; ++fx, --bx) {
            tmp = cmdtbl->tags[bx];
            cmdtbl->tags[bx] = cmdtbl->tags[fx];
            cmdtbl->tags[fx] = tmp;
        }

        while (*pp) {
	    switch (tag_cmp(cmdtbl, *pp)) {
	      case TAGS_EQ:
                cc_err(cc, "command table entries with identical keys");
		return NULL;

	      case TAGS_GT:
		pp = &(*pp)->right;
		continue;

	      case TAGS_LT:
		break;
	    }
	    break;
        }
	cmdtbl->right = *pp;
	*pp = cmdtbl;
	pp  = &head;
        cmdtbl++;
	cmds++;
    }

    /*
     * compute the required depth of the tree, and the number of
     * unpopulated leaf nodes
     */
    depth = 1;
    empty = 1;
    cmdbits = cmds;
    while (cmdbits >>= 1) {
	empty += 1;
	depth++;
    }
    empty = (1 << empty) - (1 + cmds);

    /*
     * remember the tree root in the end-of-table entry
     */
    cmdtbl->left = tbltree(&head, depth, &empty);

    return cmdtbl->left;
}


/*
 * register and remember a CDB subscription point
 */
boolean
qc_subscribe (qc_confd_t     *cc,
              enum qc_subtype subtype,
              int             flags,
              int             priority,
              int             namespace,
              cmdtbl_t       *cmdtbl,
              const char     *fmt, ...)
{
    struct subdata *sdp;
    uint32_t sub2_flags, iter_flags;
    va_list args;

    /*
     * check if the cmdtbl has already been threaded (i.e. it's being
     * used for multiple subscriptions
     */
    if (cmdtbl->left  == NULL &&
        cmdtbl->right == NULL) {
        cmdtbl = cmdtbl_thread(cc, cmdtbl);
        if (cmdtbl == NULL) {
            return FALSE;
        }
    } else {
        /*
         * otherwise we need to find the tree root, which we remember
         * in the last entry
         */
        while (cmdtbl->tags[0]) {
            cmdtbl++;
        }
        cmdtbl = cmdtbl->left;
    }

    if (cc->nsubpts >= N_QC_SUBPOINTS) {
        cc_err(cc, "too many subscriptions");
        return FALSE;
    }
    sdp = &cc->subdata[cc->nsubpts];

    va_start(args, fmt);
    if (vsnprintf(sdp->path, sizeof(sdp->path),
                  fmt, args) >= sizeof(sdp->path)) {
        va_end(args);
        cc_err(cc, "subscription path too long: %s", sdp->path);
        return FALSE;
    }
    va_end(args);

    /*
     * translate QC flags into subscribe/iterate flags
     */
    sub2_flags = 0;                     // none of these defined yet
    iter_flags = 0;
    if (flags & QC_SUB_ANCESTOR_DELETE) {
        iter_flags |= ITER_WANT_ANCESTOR_DELETE;
    }

    sdp->subtype      = subtype;
    sdp->priority     = priority;
    sdp->sub2_flags   = sub2_flags;
    sdp->iter_flags   = iter_flags;
    sdp->namespace    = namespace;
    sdp->qc_sub_flags = flags;
    sdp->cmdtree      = cmdtbl;
    sdp->cc           = cc;

    if (! cc_reg_subpt(cc, sdp, &cc->subpts[cc->nsubpts])) {
        return FALSE;
    }

    cc->nsubpts++;

    return TRUE;
}


/*
 * indicate to confd we're done with subscriptions
 */
boolean
qc_subscribe_done (qc_confd_t *cc)
{
    if (cc->nsubpts == 0) {
        return TRUE;
    }

    if (cdb_subscribe_done(cc->subsock) != CONFD_OK) {
        cc_err(cc, "subscribe_done: %s", confd_last_err());
        return FALSE;
    }

    return TRUE;
}


/*
 * service confd callback data on a control or worker socket
 */
boolean
qc_handle_cb (qc_confd_t *cc,
              int         sock)
{
    switch (confd_fd_ready(cc->dctx, sock)) {
      case CONFD_OK:
        break;

      case CONFD_ERR:
        if (confd_errno == CONFD_ERR_EXTERNAL) {
            /*
             * not a confd problem; a callback returned CONFD_ERR
             */
            break;
        }
        cc_err(cc, "confd_fd_ready(%u): %s", sock, confd_last_err());
        return FALSE;

      case CONFD_EOF:
        cc_err(cc, "confd_fd_ready(%u): EOF", sock);
        return FALSE;
    }

    return TRUE;
}


/*
 * return -1/0/+1 indicating whether the tags in "kp" (between
 * kx_start & kx_end) are lt/eq/gt those in "tags[]".  The length of a
 * partial match is indicated by "*partial"
 */
static int
tagmatch (confd_hkeypath_t *kp,
          int               kx_start,
          int               kx_end,
          uint32_t          tags[],
          int              *partial)
{
    int kx = kx_start;
    int tx, retv;
    uint32_t tag;

    // default return value indicates a match
    retv = 0;

    tx = 0;
    for (;;) {
        tag = kp->v[kx][0].val.xmltag.tag;
        if (tag < tags[tx]) {
            retv = -1;
            break;
        }
        if (tag > tags[tx]) {
            retv = 1;
            break;
        }

        // if there are no more tags in the pattern, we're done
        if (++tx     == N_CMD_TAGS ||
            tags[tx] == 0) {
            break;
        }

        // find the next XML tag in the path
        while (++kx < kx_end) {
            if (kp->v[kx][0].type == C_XMLTAG) {
                break;
            }
        }

        // if we run out of path components before running out of pattern
        // look left for a shorter match
        if (kx == kx_end) {
            retv = -1;
            break;
        }
    } 
    *partial = tx;

    // dump match results to stderr when debugging
    if (qc_debug) {
        int ix;
        char buf[512];
        uint32_t kp_ns = 0;

        fprintf(stderr, "%2d, %d, %d <= tagmatch(", retv, *partial, kx_start);
        kx = kx_start;
        while (kx < kx_end) {
            if (kp->v[kx][0].type == C_XMLTAG) {
                kp_ns = kp->v[kx][0].val.xmltag.ns;
                confd_pp_value(buf, sizeof(buf), &kp->v[kx][0]);
                fprintf(stderr, "%s ", buf);
            }
            kx++;
        }
        fprintf(stderr, ":");
        for (ix = 0; ix < N_CMD_TAGS && tags[ix]; ++ix) {
            confd_value_t v;

            CONFD_SET_XMLTAG(&v, tags[ix], kp_ns);
            confd_pp_value(buf, sizeof(buf), &v);
            fprintf(stderr, " %s", buf);
        }
        fprintf(stderr, ")\n");
    }

    return retv;
}


/*
 * search cmdtree for a match to kp (bounded by the path length in *len)
 * return the number of XML tags match in *len, and the XML tag level in
 * the input path at which the match occurred
 */
static cmdtbl_t*
match_cmd (confd_hkeypath_t *kp,
           int              *len,
           cmdtbl_t         *cmdtree,
           int              *taglevel)
{
    cmdtbl_t *cmd;
    int ix, kp_len, tags;

    kp_len  = *len;
    /*
     * for each XML tag in the path, starting with the lowest level tag,
     * search the command table for an entry with a matching sequence of
     * XML tags.  Return the longest match at the first level with any
     * match.
     */
    *len = 0;
    for (ix = 0, tags = 0; ix < kp_len; ++ix) {
        // find the next tag
	if (kp->v[ix][0].type == C_XMLTAG) {
            ++tags;
            cmd = cmdtree;
            while (cmd) {
                int cmp, partial;

                cmp = tagmatch(kp, ix, kp_len, cmd->tags, &partial);
                if (partial > *len) {
                    *len      = partial;
                    *taglevel = tags - 1;
                }

                // keep looking to the left or right...
                switch (cmp) {
                case -1:
                    cmd = cmd->left;
                    continue;

                case  1:
                    cmd = cmd->right;
                    continue;
                }

                // ...or return full match indication
                return cmd;
            }

            // no exact match, but partial match at this level, so
            // return indication of that
            if (*len) {
                break;
            }
        }
    }

    return NULL;
}


/*
 * find the node in cmdtree with the best match to the tag path in kp
 * "best" means matching at the lowest level tag in the path,
 * and at that level, the longest sequence of tags.
 */
static cmdtbl_t*
find_cmd (confd_hkeypath_t *kp,
          cmdtbl_t         *cmdtree)
{
    int len, level;
    cmdtbl_t *cmd, *best;

    len = kp->len;
    if (qc_debug) fprintf(stderr, "match_cmd1(%u)\n", len);
    best = cmd = match_cmd(kp, &len, cmdtree, &level);
    // while cmd->tags matched something
    while (cmd) {
        cmdtbl_t *cmd1;
        int len1, level1;

        // done if no longer matches are possible
        if (cmd->right == NULL) {
            break;
        }

        // otherwise, look right for a longer match
        len1   = kp->len;
        if (qc_debug) fprintf(stderr, "match_cmd2(%u)\n", len1);
        cmd1 = match_cmd(kp, &len1, cmd->right, &level1);

        // if we found a longer match (at the same level),
        // remember how long and where
        if (len1    > len &&
            level1 == level) {
            len = len1;
            cmdtree = cmd->right;
            if (cmd1) {
                best = cmd1;
            }
        }

        cmd = cmd1;
    }

    return best;
}


/*
 * iterate over changes to CDB
 */
static enum cdb_iter_ret
cc_iter_diffs (confd_hkeypath_t *kp,
               enum cdb_iter_op  op,
               confd_value_t    *oldv,           // requires ITER_WANT_PREV
               confd_value_t    *newv,
               void             *state)
{
    char buf[512];
    cmdtbl_t *cmd;
    struct subdata *sdp;

    sdp = state;

    // dump diff notification when tracing
    if (sdp->cc->log_level == CONFD_TRACE) {
        qc_pp_change(buf, sizeof(buf), kp, op, newv, sdp->namespace);
        fprintf(sdp->cc->log_stream, "sub@%s: %s\n", sdp->path, buf);
    }

    /*
     * ignore modification events (leaving create, set, delete)
     */
    if (op == MOP_MODIFIED) {
	return ITER_RECURSE;
    }

    cmd = find_cmd(kp, sdp->cmdtree);
    if (cmd) {
        qc_confd_t *cc;
        enum cdb_iter_ret retv;

        /*
         * ignore NULL handler
         */
        if (cmd->handler == NULL) {
            if (qc_debug) fprintf(stderr, "cmd ignored\n");
            return ITER_RECURSE;
        }

        /*
         * recurse unless a prepare fails
         */
        retv = ITER_RECURSE;
        cc   = sdp->cc;
        if (cc->app_lock) {
            cc->app_lock();
        }
        if (! cmd->handler(kp, op, newv, sdp->notify_type, cmd->which)) {
            if (sdp->notify_type == CDB_SUB_PREPARE) {
                retv = ITER_STOP;
                sdp->prepare_failed   = TRUE;
                sdp->prepare_fail_tag = qc_get_xmltag(kp, 1);
            } else {
                /// better report unexpected command failure?
                qc_pp_change(buf, sizeof(buf), kp, op, newv, sdp->namespace);
                fprintf(cc->log_stream, "cmd fail: %s\n", buf);
            }
        }
        if (cc->app_unlock) {
            cc->app_unlock();
        }

        return retv;
    }

    /// better report unhandled command?
    qc_pp_change(buf, sizeof(buf), kp, op, newv, sdp->namespace);
    fprintf(sdp->cc->log_stream, "unhandled cmd: %s\n", buf);

    return ITER_CONTINUE;
}


/*
 * send notification of "end of diffs" by invoking a pseudo command
 * with a null keypath
 */
static void
cc_eod_notify (struct subdata *sdp)
{
    cmdtbl_t *cmd;
    confd_hkeypath_t kp;

    kp.len = 1;
    CONFD_SET_XMLTAG(&kp.v[0][0], qc_eod_notify, sdp->namespace);
    cmd = find_cmd(&kp, sdp->cmdtree);
    if (cmd && cmd->handler) {
        if (! cmd->handler(NULL,              // no hkeypath
                           MOP_MODIFIED,
                           NULL,              // no new value
                           sdp->notify_type,
                           cmd->which)) {
            if (sdp->notify_type == CDB_SUB_PREPARE) {
                sdp->prepare_failed   = TRUE;
                sdp->prepare_fail_tag = 0;
            }
        }
    }

    return;
}


/*
 * handle subscription events
 */
static boolean
cc_handle_sub (qc_confd_t *cc)
{
    boolean retv;
    int subix, nsubev, *subev;
    enum cdb_sub_notification sub_notify_type;
    boolean  prepare_failed;
    uint32_t prepare_fail_tag;

    /*
     * read subscription events
     */
    if (cdb_read_subscription_socket2(cc->subsock,
                                      &sub_notify_type,
                                      NULL,     // no interest in flags
                                      &subev,
                                      &nsubev) != CONFD_OK) {
        cc_err(cc, "cdb_read_subsock2: %s", confd_last_err());
        return FALSE;
    }

    /*
     * process only commit events (for now?)
     */
    retv = TRUE;

    prepare_failed = FALSE;
    for (subix = 0; subix < nsubev; ++subix) {
        int ix, subpt;
        struct subdata *sdp;

        subpt = subev[subix];

        /*
         * find our subscription data for this subscription point
         */
        sdp = NULL;
        for (ix = 0; ix < cc->nsubpts; ++ix) {
            if (subpt == cc->subpts[ix]) {
                sdp = &cc->subdata[ix];
                break;
            }
        }
        if (sdp == NULL) {
            cc_err(cc, "unknown subpt %d", subpt);
            retv = FALSE;
            break;
        }

        sdp->notify_type    = sub_notify_type;
        sdp->prepare_failed = FALSE;
        if (cdb_diff_iterate(cc->subsock, subpt,
                             cc_iter_diffs,
                             sdp->iter_flags,
                             sdp) != CONFD_OK) {
            cc_err(cc, "cdb_diff_iterate(%d): %s",
                   subpt, confd_last_err());
            retv = FALSE;
            break;
        }

        if (sdp->qc_sub_flags & QC_SUB_EOD_NOTIFY) {
            cc_eod_notify(sdp);
        }

        if (sdp->prepare_failed) {
            prepare_failed   = TRUE;
            prepare_fail_tag = sdp->prepare_fail_tag;
            break;
        }
    }

    free(subev);

    if (prepare_failed) {
        if (cdb_sub_abort_trans(cc->subsock,
                                CONFD_ERRCODE_INCONSISTENT_VALUE,
                                0,      // confd future: use sdp->namespace
                                prepare_fail_tag,
                                "bad value") != CONFD_OK) {
            cc_err(cc, "abort_trans: %s", confd_last_err());
            retv = FALSE;
        }
    } else {
        if (cdb_sync_subscription_socket(cc->subsock,
                                         (sub_notify_type == CDB_SUB_OPER) ?
                                         CDB_DONE_OPERATIONAL :
                                         CDB_DONE_PRIORITY) != CONFD_OK) {
            cc_err(cc, "subsock sync: %s", confd_last_err());
            retv = FALSE;
        }
    }

    return retv;
}


/*
 * blocking thread to poll confd sockets and handle events
 * returns only on error
 * handles only a single worker socket
 *
 * or, an app can handle polling itself:
 *   cdb.subsock - call qc_handle_sub
 *   cdb.ctlsock & cdb.wrksock(s) - call qc_handle_cb
 */
void*
qc_confd_poll (void* arg)
{
    qc_confd_t *cc;
    struct pollfd set[2 + N_QC_WRKSOCKS];
    int poll_fail_counter;
    int nfd;

    cc = arg;
    nfd = 0;

    if (cc->nsubpts) {
        set[nfd].fd     = cc->subsock;
        set[nfd].events = POLLIN;
        nfd++;
    }

    set[nfd].fd     = cc->ctlsock;
    set[nfd].events = POLLIN;
    nfd++;

    set[nfd].fd     = cc->wrksock[0];
    set[nfd].events = POLLIN;
    nfd++;

    poll_fail_counter = 0;
    while (1) {
        int ix;

	if (poll(&set[0], nfd, -1) < 0) {
	    if (++poll_fail_counter < 10)
		continue;
	    cc_err(cc, "Excessive poll failures");
            return int2ptr(FALSE);
	}
	poll_fail_counter = 0;

        for (ix = 0; ix < nfd; ++ix) {
            if (set[ix].revents & ~POLLIN) {
                cc_err(cc, "unexpected poll event: %x, fd %d",
                       set[ix].revents, ix);
                return int2ptr(FALSE);
            }

            if ((set[ix].revents & POLLIN) == 0) {
                continue;
            }

            if (ix == 0 && cc->nsubpts > 0) {
                if (! cc_handle_sub(cc)) {
                    return int2ptr(FALSE);
                }

                /// this hangs during startup config processing
                if (0) {
                    /*
                     * remember last processed transaction for catching up
                     * after confd restart
                     */
                    if (cdb_get_txid(cc->cdbsock, &cc->txid) != CONFD_OK) {
                        /// report failure
                        fprintf(cc->log_stream,
                                "txid fail: %s\n", confd_last_err());
                    } else {
                        // print transaction id when debugging
                        if (qc_debug) {
                            fprintf(cc->log_stream, "txid %u-%u-%u\n",
                                    cc->txid.s1, cc->txid.s2, cc->txid.s3);
                        }
                    }
                }

                continue;
            }

            if (! qc_handle_cb(cc, set[ix].fd)) {
                return int2ptr(FALSE);
            }
	}
    }
}


/*
 * register a command callback
 */
boolean
qc_reg_cmdpoint (qc_confd_t  *cc,
                 char        *cmdpoint,
                 boolean    (*cmdfunc)(int                     maapisock,
                                       struct confd_user_info *uinfo,
                                       int                     argc,
                                       char                   *argv[],
                                       long                    which),
                 long         which)
{
    struct cmddata *cdp;

    if (cc->ncmdpts >= N_QC_CMDPOINTS) {
        cc_err(cc, "too many cmdpoints");
        return FALSE;
    }

    cdp = &cc->cmddata[cc->ncmdpts];

    if (snprintf(cdp->cmdpoint, sizeof(cdp->cmdpoint),
                 "%s", cmdpoint) >= sizeof(cdp->cmdpoint)) {
        cc_err(cc, "cmdpoint name too long: %s", cmdpoint);
        return FALSE;
    }

    cdp->which   = which;
    cdp->handler = cmdfunc;

    if (! cc_reg_cmdpt(cc, cdp)) {
        return FALSE;
    }

    cc->ncmdpts++;

    return TRUE;
}


/*
 * register a completion callback
 */
boolean
qc_reg_completion (qc_confd_t *cc,
                   char       *comppoint,
                   boolean   (*compfunc)
                       (struct confd_user_info *uinfo,
                        int                     cli_style,
                        char                   *token,
                        int                     comp_char,
                        confd_hkeypath_t       *kp,
                        char                   *cmdpath,
                        char                   *param_id))
{
    struct compdata *cmp;

    if (cc->ncomppts >= N_QC_COMPPOINTS) {
        cc_err(cc, "too many completion points");
        return FALSE;
    }

    cmp = &cc->compdata[cc->ncomppts];

    if (snprintf(cmp->comppoint, sizeof(cmp->comppoint),
                 "%s", comppoint) >= sizeof(cmp->comppoint)) {
        cc_err(cc, "completion name too long: %s", comppoint);
        return FALSE;
    }

    cmp->handler = compfunc;

    if (! cc_reg_comppt(cc, cmp)) {
        return FALSE;
    }

    cc->ncomppts++;

    return TRUE;
}

/*
 * register a set of transaction callbacks
 */
boolean
qc_register_trans_cb (qc_confd_t *cc, struct qc_trans_cbs *trans_cbs)
{
    if (!trans_cbs) {
        memset(&cc->trans_cbs, 0, sizeof(cc->trans_cbs));
    } else {
        memcpy(&cc->trans_cbs, trans_cbs, sizeof(cc->trans_cbs));
    }

    return TRUE;
}

/*
 * indicate to confd that all callback registrations are done
 */
boolean
qc_callbacks_done (qc_confd_t *cc)
{
    if (confd_register_done(cc->dctx) != CONFD_OK) {
        cc_err(cc, "register_done: %s", confd_last_err());
        return FALSE;
    }

    return TRUE;
}


/*
 * return a socket for read or read/write of CDB
 * - 'db' is CDB_RUNNING, CDB_STARTUP, or CDB_OPERATIONAL
 * - 'flags' may include CDB_LOCK_SESSION, CDB_LOCK_REQUEST
 *
 * blocks if a write transaction is currently running, or if
 * another client has CDB open for exclusive access
 *
 * presumably this isn't done all that frequently, so the cost of opening
 * and connecting a socket to CDB each time is acceptable (about 250 usec
 * on a build server with local confd)
 */
int
qc_open_cdb (qc_confd_t      *cc,
             enum cdb_db_type db,
             int              flags)
{
    int cdbsock;

    if (! cc_cdbsock(cc, &cdbsock)) {
        return -1;
    }

    flags |= CDB_LOCK_WAIT;
    if (cdb_start_session2(cdbsock, db, flags) != CONFD_OK) {
        cc_err(cc, "cdb_start_session: %s", confd_last_err());
        close(cdbsock);
        return -1;
    }

    return cdbsock;
}


/*
 * close a CDB socket
 */
void
qc_close_cdb (int cdbsock)
{
    cdb_end_session(cdbsock);
    cdb_close(cdbsock);
}

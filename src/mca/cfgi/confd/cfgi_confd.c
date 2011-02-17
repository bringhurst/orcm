/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

typedef unsigned int boolean;
#define TRUE 1
#define FALSE 0

#include "openrcm_config_private.h"
#include "constants.h"

#include "orcm-confd.h"
#include "q_confd.h"

#include "opal/class/opal_pointer_array.h"
#include "opal/dss/dss.h"
#include "opal/mca/event/event.h"
#include "opal/util/argv.h"
#include "opal/util/basename.h"
#include "opal/util/if.h"
#include "opal/util/opal_environ.h"
#include "opal/util/os_path.h"
#include "opal/util/output.h"
#include "opal/util/fd.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/plm/plm.h"
#include "orte/mca/plm/base/plm_private.h"
#include "orte/runtime/orte_globals.h"
#include "orte/util/context_fns.h"

#include "mca/pnp/pnp.h"
#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/base/public.h"
#include "cfgi_confd.h"

typedef struct {
    opal_object_t super;
    uint8_t cmd;
    orte_job_t *jdata;
} orcm_job_caddy_t;
static void caddy_const(orcm_job_caddy_t *ptr)
{
    ptr->jdata = NULL;
}
static void caddy_dest(orcm_job_caddy_t *ptr)
{
    if (NULL != ptr->jdata) {
        OBJ_RELEASE(ptr->jdata);
    }
}
OBJ_CLASS_INSTANCE(orcm_job_caddy_t,
                   opal_object_t,
                   caddy_const, caddy_dest);
#define ORCM_CONFD_SPAWN 0x01
#define ORCM_CONFD_KILL  0x02

static opal_pointer_array_t installed_apps;
static orte_job_t *jdata;
static orte_app_context_t *app;
static opal_mutex_t internal_lock;
static opal_condition_t internal_cond;
static bool waiting;
static bool thread_active;
static bool valid=false;
static bool modifying=false;
static bool deleting=false;
static bool initialized = false;
static bool enabled = false;
static opal_mutex_t enabled_lock;
static pthread_t confd_nanny_id;
static char **interfaces=NULL;
static bool confd_master_is_local=false;
static int launch_pipe[2];
static opal_event_t launch_event;

static void launch_thread(int fd, short sd, void *args);
static orte_job_t *get_app(char *name, bool create);
static orte_app_context_t *get_exec(orte_job_t *jdat,
                                    char *name, bool create);
static void copy_defaults(orte_job_t *target, orte_job_t *src);

static boolean cfg_handler(confd_hkeypath_t *kp,
			   enum cdb_iter_op  op,
			   confd_value_t    *value,
			   enum cdb_sub_notification notify_type,
			   long              which);

static boolean install_handler(confd_hkeypath_t *kp,
                               enum cdb_iter_op  op,
                               confd_value_t    *value,
			       enum cdb_sub_notification notify_type,
                               long              which);

static cmdtbl_t config_cmds[] = {
  { { orcm_run      }, cfg_handler, },
  { { qc_eod_notify }, cfg_handler, },
  { },
};

static cmdtbl_t install_cmds[] = {
  { { orcm_install  }, install_handler, },
  { { qc_eod_notify }, install_handler, },
  { },
};

static boolean parse(confd_hkeypath_t *kp,
                     enum cdb_iter_op  op,
                     confd_value_t    *value,
	             enum cdb_sub_notification notify_type,
		     bool install);

static int orcm_get_elem (struct confd_trans_ctx *tctx,
			  confd_hkeypath_t       *kp);

static int orcm_get_next (struct confd_trans_ctx *tctx,
			  confd_hkeypath_t       *kp,
			  long next);

/*
 * set up initial communication w/confd and register for callbacks
 * returns having requested from confd the startup config for all
 * subscription points
 *
 * NOTE: we have turned "off" two-phase commits for running and
 * installation data. This is necessary because confd only allows
 * ONE process to subscribe for validation, which would create a
 * single point-of-failure in the system. Validation will therefore
 * have to be done in a separate manner (TBD)
 */
static boolean
connect_to_confd (qc_confd_t *cc,
                  char       *log_prefix,
                  FILE       *log_file)
{
    /*
     * initialize the connection to confd
     * the last parameter is { CONFD_SILENT, CONFD_DEBUG, CONFD_TRACE }
     */
    if (1 < opal_output_get_verbosity(orcm_cfgi_base.output)) {
        if (! qc_confd_init(cc, log_prefix, log_file, CONFD_TRACE)) {
            return FALSE;
        }
    } else {
        if (! qc_confd_init(cc, log_prefix, log_file, CONFD_SILENT)) {
            return FALSE;
        }
    }

    /*
     * wait for confd to allow subscriptions
     */
    if (! qc_wait_start(cc))
        return FALSE;

    return TRUE;
}

static boolean cfgi_confd_subscribe(qc_confd_t *cc)
{
    struct confd_data_cbs data_cbs = {
        .callpoint       = "orcm_oper",

        .get_elem        = orcm_get_elem,
        .get_next        = orcm_get_next,

        // .num_instances   = ,
        // .get_object      = ,
        // .get_next_object = ,
        // .set_elem        = ,
        // .create          = ,
        // .remove          = ,
        // .exists_optional = ,
        // .get_case        = ,
        // .set_case        = ,
    };

    /*
     * register a subscription to the install defaults
     */
    if (! qc_subscribe(cc,
                       QC_SUB_CONFIG,
                       QC_SUB_EOD_NOTIFY,       /* flags */
                       0,			/* priority */
                       orcm__ns,
                       install_cmds,
                       "/config/install"))
        return FALSE;

    /*
     * register a subscription
     */
    if (! qc_subscribe(cc,
                       QC_SUB_CONFIG,
                       QC_SUB_EOD_NOTIFY,       /* flags */
                       5,			/* priority */
                       orcm__ns,
                       config_cmds,
                       "/config/run"))
        return FALSE;

    /*
     * tell confd we're done with subscriptions
     */
    if (! qc_subscribe_done(cc))
        return FALSE;

#if 0
    /*
     * register CLI command handlers
     */
    if (! qc_reg_cmdpoint(cc, "show-data", show_data_func, 0))
        return FALSE;
#endif

    /*
     *register as a data provider
     */
    if (qc_reg_data_cb(cc, &data_cbs)) {
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s CONFD OPERATIONAL DATA PROVIDER",
                              ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    } else {
        /* it doesn't matter who provides this service,
         * so it isn't an error if someone else got
         * there first - just note it if requested
         */
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s NOT CONFD OPERATIONAL DATA PROVIDER",
                              ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    }

    /*
     * indicate done with callback registrations
     */
    if (! qc_callbacks_done(cc))
        return FALSE;

    /*
     * ask confd to give us our startup config
     */
    if (! qc_startup_config(cc))
        return FALSE;

    return TRUE;
}


/*
 * thread to maintain a connection to confd
 */
static void*
confd_nanny (void *arg)
{
    /*
     * confd interface state
     */
    qc_confd_t cc;
    int num_tries=0;
    struct timespec delay = {0, 1000};
    int idx;
    char log_pfx[48];

    snprintf(log_pfx, sizeof(log_pfx), "ORCM-DVM/%d", \
             (int)ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid));

    /*
     * retry the connection setup a couple of times
     * in case there is a race condition
     */
    while (! connect_to_confd(&cc, log_pfx, stderr)) {
        if (1 == num_tries) {
            /* we tried it twice - time to punt */
            thread_active = false;
            waiting = false;
            opal_condition_signal(&internal_cond);
            return NULL;
        }
        num_tries++;
        nanosleep(&delay, NULL);
        qc_close(&cc);
    }
    thread_active = true;
    waiting = false;
    opal_condition_signal(&internal_cond);

    if (FALSE == cfgi_confd_subscribe(&cc)) {
        opal_output(0, "%s FAILED TO SUBSCRIBE TO CONFD",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        thread_active = false;
        return NULL;
    }
    /*
     * loop forever handling events, and reconnecting when needed
     */
    num_tries = 0;
    for (;;) {
        /*
         * handle events from confd (including the startup config events)
         * this returns only on error
         */
        qc_confd_poll(&cc);
        do {
            num_tries++;
            if (5 < num_tries) {
                thread_active = false;
                return NULL;
            }
            qc_close(&cc);
            sleep(1);
        } while (! qc_reconnect(&cc));
        num_tries = 0;
    }

    return NULL;
}


static int cfgi_confd_init(void)
{
    /* if we already did this, don't do it again */
    if (initialized) {
        return ORCM_SUCCESS;
    }

    OBJ_CONSTRUCT(&installed_apps, opal_pointer_array_t);
    opal_pointer_array_init(&installed_apps, 16, INT_MAX, 16);

    OBJ_CONSTRUCT(&internal_lock, opal_mutex_t);
    OBJ_CONSTRUCT(&internal_cond, opal_condition_t);
    OBJ_CONSTRUCT(&enabled_lock, opal_mutex_t);
    waiting = true;
    thread_active = false;
    valid = true;
    initialized = true;
    enabled = false;

    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output, "cfgi:confd initialized"));
    return ORCM_SUCCESS;
}

static void cfgi_confd_activate(void)
{
    if (enabled) {
        return;
    }

    /* if we are the lowest vpid alive, we make the
     * connection to confd
     */
    if (ORTE_PROC_MY_NAME->vpid == orte_get_lowest_vpid_alive(ORTE_PROC_MY_NAME->jobid)) {
        /* setup the launch event */
        if (pipe(launch_pipe) < 0) {
            opal_output(0, "CANNOT OPEN LAUNCH PIPE");
            return;
        }
        opal_event_set(opal_event_base, &launch_event, launch_pipe[0],
                       OPAL_EV_READ|OPAL_EV_PERSIST, launch_thread, NULL);
        opal_event_add(&launch_event, 0);
        /* start the connection */
        OPAL_THREAD_LOCK(&internal_lock);
        if (pthread_create(&confd_nanny_id,
                           NULL,            /* thread attributes */
                           confd_nanny,
                           NULL             /* thread parameter */) < 0) {
            /* if we can't start a thread, then just return */
            OPAL_THREAD_UNLOCK(&internal_lock);
            return;
        }
        while (waiting) {
            opal_condition_wait(&internal_cond, &internal_lock);
        }
        if (!thread_active) {
            /* nobody answered the phone */
            waiting = true;
            opal_event_del(&launch_event);
            OPAL_THREAD_UNLOCK(&internal_lock);
            return;
        }
        OPAL_THREAD_UNLOCK(&internal_lock);
    } else {
        /* do nothing - data will be sent to the tool component */
        return;
    }

    enabled = true;
}

static int cfgi_confd_finalize(void)
{
    int i;
    orte_job_t *jd;

    if (initialized) {
        OBJ_DESTRUCT(&internal_lock);
        OBJ_DESTRUCT(&internal_cond);
        OBJ_DESTRUCT(&enabled_lock);

        for (i=0; i < installed_apps.size; i++) {
            if (NULL != (jd = (orte_job_t*)opal_pointer_array_get_item(&installed_apps, i))) {
                OBJ_RELEASE(jd);
            }
        }
        OBJ_DESTRUCT(&installed_apps);

        opal_argv_free(interfaces);

        if (thread_active) {
            opal_event_del(&launch_event);
        }

        initialized = false;
    }
    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output, "cfgi:confd finalized"));
    return ORCM_SUCCESS;
}


orcm_cfgi_base_module_t orcm_cfgi_confd_module = {
    cfgi_confd_init,
    cfgi_confd_finalize,
    cfgi_confd_activate
};

static void launch_thread(int fd, short sd, void *args)
{
    int rc;
    orcm_job_caddy_t *caddy;

    opal_fd_read(launch_pipe[0], sizeof(orcm_job_caddy_t*), &caddy);


    if (ORCM_CONFD_SPAWN == caddy->cmd) {
        /* launch it */
        if (ORCM_SUCCESS != (rc = orcm_cfgi_base_spawn_app(caddy->jdata, true))) {
            ORTE_ERROR_LOG(rc);
            opal_output(0, "SPECIFIED APP %s IS INVALID - IGNORING",
                        (NULL == caddy->jdata->name) ? "NULL" : caddy->jdata->name);
            /* remove the job from the global pool */
            opal_pointer_array_set_item(orte_job_data, ORTE_LOCAL_JOBID(caddy->jdata->jobid), NULL);
        } else {
            /* if we were successful, then this job object is now
             * in the global array, so protect it here
             */
            caddy->jdata = NULL;
        }
    } else if (ORCM_CONFD_KILL == caddy->cmd) {
        if (ORCM_SUCCESS != (rc = orcm_cfgi_base_kill_app(caddy->jdata))) {
            ORTE_ERROR_LOG(rc);
        }
        /* must NOT release the job data object at this time - we
         * need it for when the proc states are reported!
         */
        caddy->jdata = NULL;
    } else {
        opal_output(0, "%s Unrecognized confd cmd",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    }
    OBJ_RELEASE(caddy);
}

static boolean cfg_handler(confd_hkeypath_t *kp,
                           enum cdb_iter_op  op,
                           confd_value_t    *value,
			   enum cdb_sub_notification notify_type,
                           long              which)
{
    return parse(kp, op, value, notify_type, false);
}

static void reset_globals(void)
{
    jdata = NULL;
    app = NULL;
    modifying = false;
    deleting = false;
    valid = false;
}

static boolean parse(confd_hkeypath_t *kp,
                     enum cdb_iter_op  op,
                     confd_value_t    *value,
	             enum cdb_sub_notification notify_type,
		     bool install)
{
    confd_value_t *clist, *vp;
    char *cptr, *param, *ctmp;
    unsigned int i, imax;
    int32_t i32;
    int rc, j;
    orte_job_t *jdat;
    boolean ret=FALSE;
    orcm_job_caddy_t *caddy;

    if (NULL == kp) {
        /* process the cmd */
        if (NULL != jdata) {
            opal_output_verbose(2, orcm_cfgi_base.output,
                                "event completed: %s", install ? "INSTALL" : "RUN");
            if (CDB_SUB_PREPARE == notify_type) {
                opal_output(0, "NOTIFY: PREPARE");
                /* check to see if all required elements
                 * of the job object have been provided
                 */
                if (ORCM_SUCCESS == orcm_cfgi_base_check_job(jdata)) {
                    opal_output(0, "PREPARE OKAY");
                    OBJ_RELEASE(jdata);
                    reset_globals();
                    ret = TRUE;
                    goto release;
                }
                /* nope - missing something, so notify failure */
                opal_output(0, "PREPARE FAILED");
                OBJ_RELEASE(jdata);
                reset_globals();
                ret = FALSE;
                goto release;
            } else if (CDB_SUB_ABORT == notify_type) {
                opal_output(0, "NOTIFY: ABORT - ignoring");
                OBJ_RELEASE(jdata);
                reset_globals();
                ret = TRUE;
                goto release;
            } else if (CDB_SUB_COMMIT == notify_type) {
                if (deleting) {
                    /* just reset the globals and ignore */
                    reset_globals();
                    goto release;
                }                    
                /* do a basic validity check on the job */
                if (ORCM_SUCCESS == orcm_cfgi_base_check_job(jdata)) {
                    valid = true;
                } else {
                    valid = false;
                }
                if (valid) {
                    if (install) {
                        if (mca_orcm_cfgi_confd_component.test_mode) {
                            opal_output_verbose(2, orcm_cfgi_base.output,
                                                "NOTIFY: INSTALLING");
                            /* display the result */
                            opal_dss.dump(0, jdata, ORTE_JOB);
                        }
                        /* add this to the installed data array */
                        opal_pointer_array_add(&installed_apps, jdata);
                        /* protect that data */
                        reset_globals();
                    } else {
                        /*spawn this job */
                        if (NULL == jdata) {
                            opal_output(0, "ERROR: SPAWN A NULL JOB");
                        } else {
                            if (mca_orcm_cfgi_confd_component.test_mode) {
                                opal_output(0, "SPAWNING %s", jdata->name);
                                /* display the result */
                                opal_dss.dump(0, jdata, ORTE_JOB);
                            } else {
                                /* need to get out of this thread and into one
                                 * that is running the event library
                                 */
                                caddy = OBJ_NEW(orcm_job_caddy_t);
                                caddy->cmd = ORCM_CONFD_SPAWN;
                                caddy->jdata = jdata;
                                opal_fd_write(launch_pipe[1], sizeof(orcm_job_caddy_t*), &caddy);
                            }
                            /* reinitialize globals */
                            reset_globals();
                        }
                    }
                    ret = TRUE;
                } else {
                    opal_output(0, "SPECIFIED APP %s IS INVALID - IGNORING",
                                (NULL == jdata->name) ? "NULL" : jdata->name);
                    OBJ_RELEASE(jdata);
                    /* reinitialize globals */
                    reset_globals();
                    ret = FALSE;
                }
                goto release;
            } else {
                opal_output(0, "NOTIFY: UNKNOWN");
                reset_globals();
                ret = TRUE;
                goto release;
            }
        }
        reset_globals();
        ret = TRUE;
        goto release;
    }
    
    switch (op) {
    case MOP_CREATED:
        opal_output_verbose(2, orcm_cfgi_base.output, "CREATED_OP");
        switch(CONFD_GET_XMLTAG(&kp->v[1][0])) {
        case orcm_app:
            if (install) {
                /* if a job is already active, save it as this
                 * equates to a "commit" on the prior job
                 */
                if (NULL != jdata) {
                    /* check validity */
                    if (ORCM_SUCCESS != orcm_cfgi_base_check_job(jdata)) {
                        opal_output(0, "SPECIFIED APP %s IS INVALID - IGNORING",
                                    (NULL == jdata->name) ? "NULL" : jdata->name);
                        OBJ_RELEASE(jdata);
                        /* reinitialize globals */
                        reset_globals();
                        ret = FALSE;
                        goto release;
                    }
                    opal_pointer_array_add(&installed_apps, jdata);
                    opal_output_verbose(2, orcm_cfgi_base.output,
                                        "installed application: %s", jdata->name);
                    /* protect that data */
                    jdata = NULL;
                }
                /* new job */
                jdata = OBJ_NEW(orte_job_t);
                vp = qc_find_key(kp, orcm_app, 0);
                if (NULL == vp) {
                    opal_output(0, "ERROR: BAD APP KEY");
                    OBJ_RELEASE(jdata);
                    reset_globals();
                    ret = FALSE;
                    break;
                }
                jdata->name = strdup(CONFD_GET_CBUFPTR(vp));
                opal_output_verbose(2, orcm_cfgi_base.output,
                                    "created new application: %s", jdata->name);
                ret = TRUE;
            } else {
                /* disallowed */
                opal_output(0, "RUN CONFIG ATTEMPTING TO CREATE NEW APP");
                reset_globals();
                ret = FALSE;
            }
            break;
        case orcm_exec:
            if (install) {
                /* new application */
                if (NULL == jdata) {
                    opal_output(0, "NO ACTIVE JOB FOR VALUE SET");
                    app = NULL;
                    ret = FALSE;
                    break;
                }
            
                app = OBJ_NEW(orte_app_context_t);
                vp = qc_find_key(kp, orcm_exec, 0);
                if (NULL == vp) {
                    opal_output(0, "ERROR: BAD EXEC KEY");
                    app = NULL;
                    ret = FALSE;
                    break;
                }
                app->name = strdup(CONFD_GET_CBUFPTR(vp));
                opal_output_verbose(2, orcm_cfgi_base.output, "NEW EXECUTABLE %s", app->name);
                app->idx = jdata->num_apps;
                jdata->num_apps++;
                opal_pointer_array_set_item(jdata->apps, app->idx, app);
                ret = TRUE;
            } else {
                /* modifying an existing app - find it */
                if (NULL == jdata) {
                    /* something is wrong */
                    opal_output(0, "RUN CONFIG: MODIFYING APP FOR UNSPECIFIED JOB");
                    app = NULL;
                    ret = FALSE;
                    break;
                }
                vp = qc_find_key(kp, orcm_exec, 0);
                if (NULL == vp) {
                    opal_output(0, "ERROR: BAD EXEC KEY");
                    app = NULL;
                    ret = FALSE;
                    break;
                }
                cptr = CONFD_GET_CBUFPTR(vp);
                for (j=0; j < jdata->apps->size; j++) {
                    if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, j))) {
                        continue;
                    }
                    if (0 == strcmp(app->name, cptr)) {
                        /* found the specified app */
                        ret = TRUE;
                        goto release;
                    }
                }
                /* if we get here, the app wasn't found */
                app = NULL;
                opal_output(0, "RUN CONFIG: SPECIFIED EXECUTABLE %s NOT FOUND", cptr);
                ret = FALSE;
            }
            break;
        case orcm_app_instance:
            if (install) {
                /* illegal operation */
                opal_output(0, "INSTALL CONFIG: ATTEMPTING TO INSTALL INSTANCE");
                ret = FALSE;
                break;
            }
            /* run an instance of an installed app */
            if (NULL != jdata) {
                /* check validity */
                if (ORCM_SUCCESS != orcm_cfgi_base_check_job(jdata)) {
                    opal_output(0, "SPECIFIED APP %s IS INVALID - IGNORING",
                                (NULL == jdata->name) ? "NULL" : jdata->name);
                    OBJ_RELEASE(jdata);
                    /* reinitialize globals */
                    reset_globals();
                    ret = FALSE;
                    goto release;
                }
                /* spawn the prior instance */
                caddy = OBJ_NEW(orcm_job_caddy_t);
                caddy->cmd = ORCM_CONFD_SPAWN;
                caddy->jdata = jdata;
                opal_fd_write(launch_pipe[1], sizeof(orcm_job_caddy_t*), &caddy);
                /* reinitialize globals */
                reset_globals();
            }
            jdata = OBJ_NEW(orte_job_t);
            vp = qc_find_key(kp, orcm_app_instance, 0);
            if (NULL == vp) {
                opal_output(0, "ERROR: BAD APP-INSTANCE  KEY");
                ret = FALSE;
                break;
            }
            jdata->instance = strdup(CONFD_GET_CBUFPTR(vp));
            opal_output_verbose(2, orcm_cfgi_base.output, "created app-instance: %s", jdata->instance);
            ret = TRUE;
            break;
        default:
            opal_output(0, "BAD CREATE CMD");
            ret = FALSE;
            break;
        }
        break;
    case MOP_MODIFIED:
        /* modify a pre-existing object - could require creation */
        switch(CONFD_GET_XMLTAG(&kp->v[1][0])) {
        case orcm_app:
            opal_output_verbose(2, orcm_cfgi_base.output, "MODIFY APP");
            ret = FALSE;
            break;
        case orcm_exec:
            opal_output_verbose(2, orcm_cfgi_base.output, "MODIFY EXEC");
            ret = FALSE;
            break;
        case orcm_app_instance:
            opal_output_verbose(2, orcm_cfgi_base.output, "MODIFY APP-INSTANCE");
            ret = FALSE;
            break;
        default:
            opal_output_verbose(2, orcm_cfgi_base.output, "BAD MODIFY CMD");
            ret = FALSE;
            break;
        }
        break;
    case MOP_DELETED:
        if (install) {
            vp = qc_find_key(kp, orcm_app, 0);
            if (NULL == vp) {
                /* delete all installed apps */
                for (j=0; j < installed_apps.size; j++) {
                    if (NULL == (jdat = (orte_job_t*)opal_pointer_array_get_item(&installed_apps, j))) {
                        continue;
                    }
                    opal_pointer_array_set_item(&installed_apps, j, NULL);
                    opal_output(0, "%s DELETING INSTALLED APPLICATION %s",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                (NULL == jdat->name) ? "NULL" : jdat->name);
                    OBJ_RELEASE(jdat);
                }
                goto killall_running;
                break;
            }
            /* delete only the specified installed app */
            cptr = CONFD_GET_CBUFPTR(vp);
            for (j=0; j < installed_apps.size; j++) {
                if (NULL == (jdat = (orte_job_t*)opal_pointer_array_get_item(&installed_apps, j))) {
                    continue;
                }
                if (0 == strcasecmp(jdat->name, cptr)) {
                    opal_pointer_array_set_item(&installed_apps, j, NULL);
                    opal_output(0, "%s DELETING INSTALLED APPLICATION %s",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                (NULL == jdat->name) ? "NULL" : jdat->name);
                    OBJ_RELEASE(jdat);
                    /* see if any instances of this are running - if so, kill them too */
                    for (j=0; j < orte_job_data->size; j++) {
                        if (NULL == (jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, j))) {
                            continue;
                        }
                        if (0 == strcasecmp(cptr, jdat->name)) {
                            jdata = OBJ_NEW(orte_job_t);
                            jdata->jobid = jdat->jobid;
                            jdata->state = ORTE_JOB_STATE_ABORT_ORDERED;  /* flag that this job is to be aborted */
                            caddy = OBJ_NEW(orcm_job_caddy_t);
                            caddy->cmd = ORCM_CONFD_KILL;
                            caddy->jdata = jdata;
                            opal_fd_write(launch_pipe[1], sizeof(orcm_job_caddy_t*), &caddy);
                        }
                    }
                    /* reinitialize globals */
                    reset_globals();
                    ret = TRUE;
                    break;
                }
            }
            break;
        } else {
            /* deleting a running job */
            vp = qc_find_key(kp, orcm_app_instance, 0);
            if (NULL == vp) {
                /* if no app-instance was given, then the user
                 * wants us to terminate all jobs
                 */
            killall_running:
                for (j=0; j < orte_job_data->size; j++) {
                    if (NULL == (jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, j))) {
                        continue;
                    }
                    jdata = OBJ_NEW(orte_job_t);
                    jdata->jobid = jdat->jobid;
                    jdata->state = ORTE_JOB_STATE_ABORT_ORDERED;  /* flag that this job is to be aborted */
                    caddy = OBJ_NEW(orcm_job_caddy_t);
                    caddy->cmd = ORCM_CONFD_KILL;
                    caddy->jdata = jdata;
                    opal_fd_write(launch_pipe[1], sizeof(orcm_job_caddy_t*), &caddy);
                }
                /* reinitialize globals */
                reset_globals();
                ret = TRUE;
                break;
            }
            /* find this in the active job array */
            jdata = OBJ_NEW(orte_job_t);
            cptr = CONFD_GET_CBUFPTR(vp);
            for (j=0; j < orte_job_data->size; j++) {
                if (NULL == (jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, j))) {
                    continue;
                }
                if (0 == strcmp(cptr, jdat->instance)) {
                    jdata->jobid = jdat->jobid;
                    break;
                }
            }
            if (ORTE_JOBID_INVALID == jdata->jobid) {
                opal_output(0, "%s Job %s not found - cannot delete",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), cptr);
                OBJ_RELEASE(jdata);
                ret = FALSE;
                break;
            }
            jdata->state = ORTE_JOB_STATE_ABORT_ORDERED;  /* flag that this job is to be aborted */
            caddy = OBJ_NEW(orcm_job_caddy_t);
            caddy->cmd = ORCM_CONFD_KILL;
            caddy->jdata = jdata;
            opal_fd_write(launch_pipe[1], sizeof(orcm_job_caddy_t*), &caddy);
            /* reinitialize globals */
            reset_globals();
            ret = TRUE;
        }
        break;
    case MOP_VALUE_SET:
        opal_output_verbose(2, orcm_cfgi_base.output, "VALUE_SET");
        /* if this is an update for an existing job, then all we receive
         * is a value set for the value being changed. We can detect this
         * by looking for NULL in the jdata and app
         */
        if (NULL == jdata) {
            /* must be a running job - we don't want to alter the
             * actual running data, so create a substitute one. We'll
             * store all the changed values there and then update
             * at the end
             */
            vp = qc_find_key(kp, orcm_app_instance, 0);
            if (NULL == vp) {
                opal_output(0, "ERROR: BAD APP-INSTANCE  KEY");
                ret = FALSE;
                break;
            }
            jdata = OBJ_NEW(orte_job_t);
            jdata->instance = strdup(CONFD_GET_CBUFPTR(vp));
            app = NULL;
            modifying = true;
        }
        if (modifying) {
            if (NULL == app) {
                /* must be altering an existing app - again, just add it
                 * to the jdata and we'll do the update later
                 */
                vp = qc_find_key(kp, orcm_exec, 0);
                if (NULL == vp) {
                    opal_output(0, "ERROR: BAD EXEC KEY");
                    ret = FALSE;
                    break;
                }
                app = OBJ_NEW(orte_app_context_t);
                app->app = strdup(CONFD_GET_CBUFPTR(vp));
                opal_pointer_array_add(jdata->apps, app);
                /* do the bookkeeping */
                opal_argv_append_nosize(&app->argv, app->app);
                jdata->num_apps++;
            } else {
                /* see if the new value still refers to the same exec */
                vp = qc_find_key(kp, orcm_exec, 0);
                if (NULL == vp) {
                    opal_output(0, "ERROR: BAD EXEC KEY");
                    ret = FALSE;
                    break;
                }
                cptr = CONFD_GET_CBUFPTR(vp);
                if (NULL == app->app) {
                    app->app = strdup(cptr);
                } else {
                    if (0 != strcmp(app->app, cptr)) {
                        /* refers to a different app - see if we already know it */
                        opal_output(0, "REFERENCE TO DIFF APP - NOT IMPLEMENTED");
                        ret = FALSE;
                        break;
                    }
                }
            }
        }
        switch(qc_get_xmltag(kp,1)) {
            /* JOB-LEVEL VALUES */
        case orcm_app_name:
            if (NULL == jdata->name) {
                jdata->name = strdup(CONFD_GET_CBUFPTR(value));
            }
            /* we require that the app have been previously installed */
            ret = FALSE;
            for (j=0; j < installed_apps.size; j++) {
                if (NULL == (jdat = (orte_job_t*)opal_pointer_array_get_item(&installed_apps, j))) {
                    continue;
                }
                if (0 != strcmp(jdat->name, jdata->name)) {
                    continue;
                }
                /* we have a match - copy over all the fields
                 * to serve as a default starting point
                 */
                opal_output_verbose(2, orcm_cfgi_base.output,
                                    "COPYING DEFAULTS FOR APP %s TO INSTANCE %s",
                                    jdata->name, jdata->instance);
                copy_defaults(jdata, jdat);
                ret = TRUE;
                break;
            }
            if (!ret) {
                /* if we get here, then the app was NOT previously installed */
                opal_output(0, "APP %s WAS NOT PREVIOUSLY INSTALLED", jdata->name);
                OBJ_RELEASE(jdata);
            }
            break;
        case orcm_gid:
            if (NULL == jdata) {
                opal_output(0, "CANNOT SET GID - NO ACTIVE JOB");
                ret = FALSE;
                break;
            }
            jdata->gid = CONFD_GET_INT32(value);
            ret = TRUE;
            break;
        case orcm_uid:
            if (NULL == jdata) {
                opal_output(0, "CANNOT SET UID - NO ACTIVE JOB");
                ret = FALSE;
                break;
            }
            jdata->uid = CONFD_GET_INT32(value);
            ret = TRUE;
            break;
        case orcm_enable_recovery:
            if (NULL == jdata) {
                opal_output(0, "CANNOT SET ENABLE RECOVERY - NO ACTIVE JOB");
                ret = FALSE;
                break;
            }
            i32 = CONFD_GET_INT32(value);
            if (0 != i32) {
                jdata->enable_recovery = true;
            } else {
                jdata->enable_recovery = false;
            }
            jdata->recovery_defined = true;
            ret = TRUE;
            break;
            /* APP-LEVEL VALUES */
        case orcm_replicas:
            if (NULL == app) {
                opal_output(0, "CANNOT SET NUM PROCS - NO ACTIVE APP");
                ret = FALSE;
                break;
            }
            app->num_procs = CONFD_GET_INT32(value);
            ret = TRUE;
            break;
        case orcm_exec_name:
            if (NULL == app) {
                opal_output(0, "CANNOT SET EXECUTABLE - NO ACTIVE APP");
                ret = FALSE;
                break;
            }
            opal_output_verbose(2, orcm_cfgi_base.output, "EXEC_NAME");
            app->app = strdup(CONFD_GET_CBUFPTR(value));
            /* get the basename and install it as argv[0] if not already provided */
            if (0 == opal_argv_count(app->argv)) {
                cptr = opal_basename(app->app);
                opal_argv_prepend_nosize(&app->argv, cptr);
                free(cptr);
            }
            ret = TRUE;
            break;
        case orcm_path:
            if (NULL == app) {
                opal_output(0, "CANNOT SET PATH - NO ACTIVE APP");
                ret = FALSE;
                break;
            }
            opal_output_verbose(2, orcm_cfgi_base.output, "PATH");
            cptr = strdup(CONFD_GET_CBUFPTR(value));
            /* if the executable hasn't been provided yet, that is an error */
            if (NULL == app->app) {
                opal_output(0, "CANNOT SET PATH - NO EXECUTABLE SET");
                ret = FALSE;
                break;
            }
            /* prepend the path to the executable */
            ctmp = app->app;
            app->app = opal_os_path(false, cptr, ctmp, NULL);
            free(ctmp);
            free(cptr);
            ret = TRUE;
            break;
        case orcm_max_restarts:
            if (NULL == app) {
                opal_output(0, "CANNOT SET MAX RESTARTS - NO ACTIVE APP");
                ret = FALSE;
                break;
            }
            app->max_restarts = CONFD_GET_INT32(value);
            /* flag that the recovery policy has been defined
             * so we don't pickup the system defaults
             */
            app->recovery_defined = true;
            ret = TRUE;
            break;
        case orcm_leader_exclude:
            /* boolean - presence means do not allow
             * this app to become leader
             */
            opal_output_verbose(2, orcm_cfgi_base.output, "ORCM_LDR_EXCLUDE");
            ret = TRUE;
            break;
        case orcm_version:
            opal_output_verbose(2, orcm_cfgi_base.output, "version: %s", CONFD_GET_CBUFPTR(value));
            ret = TRUE;
            break;
        case orcm_config_set:
            if (NULL == app) {
                opal_output(0, "CANNOT SET CONFIG ENVAR - NO ACTIVE APP");
                ret = FALSE;
                break;
            }
            cptr = CONFD_GET_CBUFPTR(value);
            param = mca_base_param_environ_variable("orcm", "confd", "config");
            opal_setenv(param, cptr, true, &app->env);
            free(param);
            ret = TRUE;
            break;
        case orcm_nodes:
            /* the equivalent to -host */
            if (NULL == app) {
                opal_output(0, "CANNOT SET HOST - NO ACTIVE APP");
                ret = FALSE;
                break;
            }
            clist = CONFD_GET_LIST(value);
            imax = CONFD_GET_LISTSIZE(value);
            for (i=0; i < imax; i++) {
                opal_argv_append_nosize(&app->dash_host, CONFD_GET_CBUFPTR(&clist[i]));
            }
            ret = TRUE;
            break;
        case orcm_argv:
            if (NULL == app) {
                opal_output(0, "CANNOT SET ARGV - NO ACTIVE APP");
                ret = FALSE;
                break;
            }
            clist = CONFD_GET_LIST(value);
            imax = CONFD_GET_LISTSIZE(value);
            for (i=0; i < imax; i++) {
                opal_argv_append_nosize(&app->argv, CONFD_GET_CBUFPTR(&clist[i]));
            }
            ret = TRUE;
            break;
        case orcm_env:
            if (NULL == app) {
                opal_output(0, "CANNOT SET ENV - NO ACTIVE APP");
                ret = FALSE;
                break;
            }
            clist = CONFD_GET_LIST(value);
            imax = CONFD_GET_LISTSIZE(value);
            for (i=0; i < imax; i++) {
                opal_argv_append_nosize(&app->env, CONFD_GET_CBUFPTR(&clist[i]));
            }
            ret = TRUE;
            break;
        case orcm_working_dir:
            if (NULL == app) {
                opal_output(0, "CANNOT SET CWD - NO ACTIVE APP");
                ret = FALSE;
                break;
            }
            app->cwd = strdup(CONFD_GET_CBUFPTR(value));
            ret = TRUE;
            break;

            /* IGNORED VALUES */
        case orcm_name:
            /* dealt with in the create when this key
             * was provided - so ignore it here
             */
            ret = TRUE;
            break;
        case orcm_instance_name:
            /* we already have this info - it was the key
             * when we got the create command, so just
             * keep what we already have
             */
            ret = TRUE;
            break;

        default:
            opal_output(0, "NON-UNDERSTOOD XML TAG");
            ret = FALSE;
            break;
        }
        break;
    default:
        opal_output(0, "WHAT THE HECK?");
        ret = FALSE;
        break;
    }

 release:
    /* deal with valid flag */
    if (valid && !ret) {
        valid = false;
    }
    return ret;
}

static boolean install_handler(confd_hkeypath_t *kp,
                               enum cdb_iter_op  op,
                               confd_value_t    *value,
			       enum cdb_sub_notification notify_type,
                               long              which)
{
  return parse(kp, op, value, notify_type, true);
}

static orte_job_t* get_job(orte_jobid_t job)
{
    orte_job_t *jdt;

    if (NULL == (jdt = orte_get_job_data_object(job))) {
        /* job not known */
        opal_output(0, "%s JOB %s NOT KNOWN",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_JOBID_PRINT(job));
        return NULL;
    }
    return jdt;
}

static orte_proc_t* get_child(orte_process_name_t *proc)
{
    orte_job_t *jdt;
    orte_proc_t *prc;

    if (NULL == (jdt = orte_get_job_data_object(proc->jobid))) {
        /* job not known */
        opal_output(0, "%s JOB %s NOT KNOWN",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_JOBID_PRINT(proc->jobid));
        return NULL;
    }

    if (NULL == (prc = (orte_proc_t*)opal_pointer_array_get_item(jdt->procs, proc->vpid))) {
        /* proc not known */
        opal_output(0, "%s PROC %s NOT KNOWN",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_NAME_PRINT(proc));
        return NULL;
    }

    return prc;
}

/*
 * return a operational data element
 */
static int orcm_get_elem (struct confd_trans_ctx *tctx,
			  confd_hkeypath_t       *kp)
{
    int index, arr_ix, ret;
    char *cp;
    confd_value_t val, *vp;
    orte_job_t *jdat;
    orte_proc_t *proc;
    orte_node_t *node;
    uint16_t ui16;
    int32_t i32;
    uint32_t ui32;
    orte_odls_job_t *jobdat;
    orte_odls_child_t *child;
    orte_process_name_t name;
    char nodename[64];
    orte_jobid_t jobid;

    /*
     * look at the first XML tag in the keypath to see which element
     * is being requested
     */
    switch (qc_get_xmltag(kp, 1)) {
    case orcm_job_id:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR JOBID - NO JOB PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        ui32 = CONFD_GET_UINT32(vp);
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR JOBID ELEMENT %u",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ui32));
        jobid = ORTE_CONSTRUCT_LOCAL_JOBID(ORTE_PROC_MY_NAME->jobid, ui32);
        CONFD_SET_UINT32(&val, jobid);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_job_name:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR JOB NAME ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR JOB NAME - NO JOB PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        jobid = ORTE_CONSTRUCT_LOCAL_JOBID(ORTE_PROC_MY_NAME->jobid, CONFD_GET_UINT32(vp));
        jdat = get_job(jobid);
        if (NULL == jdat) {
            /* job no longer exists */
            opal_output(0, "%s CONFD REQUEST FOR JOB NAME - JOB %s NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_JOBID_PRINT(jobid));
            goto notfound;
        }
        if (NULL == jdat->name) {
            CONFD_SET_CBUF(&val, "NONE", 4);
        } else {
            CONFD_SET_CBUF(&val, jdat->name, strlen(jdat->name));
        }
        confd_data_reply_value(tctx, &val);
        goto release;
        break;

    case orcm_path:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR PATH ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR PATH - NO JOB PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
             goto notfound;
        }
        /* get the referenced app_context */
        jobid = ORTE_CONSTRUCT_LOCAL_JOBID(ORTE_PROC_MY_NAME->jobid, CONFD_GET_UINT32(vp));
        jdat = get_job(jobid);
        if (NULL == jdat) {
            /* job no longer exists */
            opal_output(0, "%s CONFD REQUEST FOR PATH - JOB %s NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_JOBID_PRINT(jobid));
            goto notfound;
        }
        vp = qc_find_key(kp, orcm_app_context, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR PATH - NO APP INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        app = (orte_app_context_t*)opal_pointer_array_get_item(jdat->apps, CONFD_GET_UINT32(vp));
        if (NULL == app) {
            opal_output(0, "%s CONFD REQUEST FOR PATH - APP %d NOT FOUND IN JOB %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        CONFD_GET_UINT32(vp), ORTE_JOBID_PRINT(jdat->jobid));
            goto notfound;
        }
        if (NULL == app->name) {
            CONFD_SET_CBUF(&val, "NONE", 4);
        } else {
            CONFD_SET_CBUF(&val, app->name, strlen(app->name));
        }
        confd_data_reply_value(tctx, &val);
        goto release;
        break;

    case orcm_max_restarts:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR RESTARTS ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR MAX RESTARTS - NO JOB PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        /* get the referenced app_context */
        jobid = ORTE_CONSTRUCT_LOCAL_JOBID(ORTE_PROC_MY_NAME->jobid, CONFD_GET_UINT32(vp));
        jdat =get_job(jobid);
        if (NULL == jdat) {
            /* job no longer exists */
            opal_output(0, "%s CONFD REQUEST FOR MAX RESTARTS - JOB %s NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_JOBID_PRINT(jobid));
            goto notfound;
        }
        vp = qc_find_key(kp, orcm_app_context, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR MAX RESTARTS - NO APP INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        app = (orte_app_context_t*)opal_pointer_array_get_item(jdat->apps, CONFD_GET_UINT32(vp));
        if (NULL == app) {
            opal_output(0, "%s CONFD REQUEST FOR MAX RESTARTS - APP %d NOT FOUND IN JOB %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        CONFD_GET_UINT32(vp), ORTE_JOBID_PRINT(jdat->jobid));
            goto notfound;
        }
        i32 = app->max_restarts;
        CONFD_SET_INT32(&val, i32);
        confd_data_reply_value(tctx, &val);
        goto release;
        break;

    case orcm_app_name:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR APPNAME ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR APP NAME - NO JOB PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        /* get the referenced app_context */
        jobid = ORTE_CONSTRUCT_LOCAL_JOBID(ORTE_PROC_MY_NAME->jobid, CONFD_GET_UINT32(vp));
        jdat = get_job(jobid);
        if (NULL == jdat) {
            /* job no longer exists */
            opal_output(0, "%s CONFD REQUEST FOR APP NAME - JOB %s NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_JOBID_PRINT(jobid));
            goto notfound;
        }
        vp = qc_find_key(kp, orcm_app_context, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR APP NAME - NO APP INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        app = (orte_app_context_t*)opal_pointer_array_get_item(jdat->apps, CONFD_GET_UINT32(vp));
        if (NULL == app) {
            opal_output(0, "%s CONFD REQUEST FOR APP NAME - APP %d NOT FOUND IN JOB %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        CONFD_GET_UINT32(vp), ORTE_JOBID_PRINT(jdat->jobid));
            goto notfound;
        }
        if (NULL == app->name) {
            CONFD_SET_CBUF(&val, "UNNAMED", strlen("UNNAMED"));
        } else {
            CONFD_SET_CBUF(&val, app->name, strlen(app->name));
        }
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_pid:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR PID ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR PID - NO JOB PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.jobid = ORTE_CONSTRUCT_LOCAL_JOBID(ORTE_PROC_MY_NAME->jobid, CONFD_GET_UINT32(vp));;
        vp = qc_find_key(kp, orcm_replica, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR PID - NO REPLICA PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.vpid = CONFD_GET_UINT32(vp);
        /* find this process */
        proc = get_child(&name);
        if (NULL == proc) {
            opal_output(0, "%s CONFD REQUEST FOR PID - PROC %s NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&name));
            goto notfound;
        }
        CONFD_SET_UINT32(&val, proc->pid);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_num_restarts:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NUM RESTARTS ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR NUM RESTARTS - NO JOB PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.jobid = ORTE_CONSTRUCT_LOCAL_JOBID(ORTE_PROC_MY_NAME->jobid, CONFD_GET_UINT32(vp));;
        vp = qc_find_key(kp, orcm_replica, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR NUM RESTARTS - NO REPLICA PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.vpid = CONFD_GET_UINT32(vp);
        /* find this process */
        proc = get_child(&name);
        if (NULL == proc) {
            opal_output(0, "%s CONFD REQUEST FOR NUM RESTARTS - PROC %s NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&name));
            goto notfound;
        }
        CONFD_SET_INT32(&val, proc->restarts);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_node:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR NODE - NO JOB PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.jobid = ORTE_CONSTRUCT_LOCAL_JOBID(ORTE_PROC_MY_NAME->jobid, CONFD_GET_UINT32(vp));;
        vp = qc_find_key(kp, orcm_replica, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR NODE - NO REPLICA PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.vpid = CONFD_GET_UINT32(vp);
        /* find this process */
        proc = get_child(&name);
        if (NULL == proc) {
            opal_output(0, "%s CONFD REQUEST FOR NODE - PROC %s NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&name));
            goto notfound;
        }
        if (NULL == proc->node || NULL == proc->node->name) {
            CONFD_SET_CBUF(&val, "**", strlen("**"));
        } else {
            if (NULL == proc->node->daemon) {
                snprintf(nodename, 64, "%s[--]", proc->node->name);
            } else {
                snprintf(nodename, 64, "%s[%s]", proc->node->name, ORTE_VPID_PRINT(proc->node->daemon->name.vpid));
            }
            CONFD_SET_CBUF(&val, nodename, strlen(nodename));
        }
        confd_data_reply_value(tctx, &val);
        goto release;
        break;

    case orcm_node_id:
        CONFD_SET_CBUF(&val, "NODEID", strlen("NODEID"));
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_node_name:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE NAME ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        CONFD_SET_CBUF(&val, "NODENAME", strlen("NODENAME"));
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_state:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE STATE ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_orte_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR NODE STATE - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_INT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR NODE STATE - NODE %d NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_INT32(vp));
            goto notfound;
        }
        switch (node->state) {
        case ORTE_NODE_STATE_UP:
            CONFD_SET_CBUF(&val, "UP", strlen("UP"));
            break;
        case ORTE_NODE_STATE_DOWN:
            CONFD_SET_CBUF(&val, "DOWN", strlen("DOWN"));
            break;
        case ORTE_NODE_STATE_REBOOT:
            CONFD_SET_CBUF(&val, "REBOOTING", strlen("REBOOTING"));
            break;
        default:
            CONFD_SET_CBUF(&val, "UNKNOWN", strlen("UNKNOWN"));
        }
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_temperature:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE TEMP ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_orte_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR NODE TEMP - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_INT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR NODE TEMP - NODE %d NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_INT32(vp));
            goto notfound;
        }
        CONFD_SET_INT8(&val, 1);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_num_procs:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE NUM PROCS ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_orte_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR NODE NUM PROCS - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_INT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR NODE NUM PROCS - NODE %d NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_INT32(vp));
            goto notfound;
        }
        i32 = node->num_procs;
        CONFD_SET_INT32(&val, i32);
        confd_data_reply_value(tctx, &val);
        goto release;

    default:
        goto notfound;
    }

 notfound:
    confd_data_reply_not_found(tctx);

 release:
    return CONFD_OK;
}


/*
 * compute the key to follow 'next'
 */
static int orcm_get_next (struct confd_trans_ctx *tctx,
			  confd_hkeypath_t       *kp,
			  long next)
{
    confd_value_t key, *ky;
    int rc=CONFD_OK;
    orte_job_t *jdat;
    orte_proc_t *p;
    uint32_t app_idx;
    uint32_t i, ui32;

    switch (qc_get_xmltag(kp, 1)) {
    case orcm_job:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NEXT JOB",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        if (next == -1) {
            next = 0;
        }
	/* look for next non-NULL job site */
	for (i=next; i < orte_job_data->size; i++) {
            if (NULL != opal_pointer_array_get_item(orte_job_data, i)) {
                CONFD_SET_UINT32(&key, i);
                confd_data_reply_next_key(tctx, &key, 1, i + 1);
                goto depart;
            }
	}
        goto notfound;
        break;

    case orcm_app_context:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NEXT APP",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        ky = qc_find_key(kp, orcm_job, 0);
        if (NULL == ky) {
            opal_output(0, "APP_CTX KEY NOT FOUND");
            goto notfound;
        }
        ui32 = CONFD_GET_UINT32(ky);
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, ui32);
        if (NULL == jdat) {
            opal_output(0, "JOB %u NOT FOUND", ui32);
           goto notfound;
        }
        if (next == -1) {
            next = 0;
        }
        /* look for next non-NULL app context */
	for (i=next; i < jdat->apps->size; i++) {
            if (NULL != opal_pointer_array_get_item(jdat->apps, i)) {
                CONFD_SET_UINT32(&key, i);
                confd_data_reply_next_key(tctx, &key, 1, i + 1);
                goto depart;
            }
	}
        goto notfound;
        break;

    case orcm_replica:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NEXT REPLICA",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        ky = qc_find_key(kp, orcm_job, 0);
        if (NULL == ky) {
            goto notfound;
        }
        jdat = get_job(CONFD_GET_UINT32(ky));
        if (NULL == jdat) {
            goto notfound;
        }
        ky = qc_find_key(kp, orcm_app_context, 0);
        if (NULL == ky) {
            goto notfound;
        }
        app_idx = CONFD_GET_UINT32(ky);
        if (next == -1) {
            next = 0;
        }
        /* look for next non-NULL proc for this app */
	for (i=next; i < jdat->procs->size; i++) {
            if (NULL != (p = (orte_proc_t*)opal_pointer_array_get_item(jdat->procs, i)) &&
                p->app_idx == app_idx) {
                CONFD_SET_UINT32(&key, i);
                confd_data_reply_next_key(tctx, &key, 1, i + 1);
                goto depart;
            }
	}
        goto notfound;
        break;

    case orcm_orte_node:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NEXT NODE",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        /* look for next non-NULL node */
        if (next == -1) {
            next = 0;
        }
        for (i=next; i < orte_node_pool->size; i++) {
            if (NULL != opal_pointer_array_get_item(orte_node_pool, i)) {
                CONFD_SET_UINT32(&key, i);
                confd_data_reply_next_key(tctx, &key, 1, i+1);
                goto depart;
            }
        }
        goto notfound;
        break;

    default:
        opal_output(0, "OPER: UNRECOGNIZED OPTION");
        break;
    }

    /*
     * not found
     */
 notfound:
    confd_data_reply_next_key(tctx, NULL, -1, -1);

 depart:
    return CONFD_OK;
}

static orte_job_t *get_app(char *name, bool create)
{
    int i;
    orte_job_t *jdat;

    for (i=0; i < installed_apps.size; i++) {
        if (NULL == (jdat = (orte_job_t*)opal_pointer_array_get_item(&installed_apps, i))) {
            continue;
        }
        if (0 == strcmp(name, jdat->name)) {
            return jdata;
        }
    }

    /* get here if not found - create if requested */
    if (!create) {
        return NULL;
    }

    jdat = OBJ_NEW(orte_job_t);
    jdat->name = strdup(name);
    opal_pointer_array_add(&installed_apps, jdat);
    return jdat;
}

static orte_app_context_t *get_exec(orte_job_t *jdat,
                                    char *name, bool create)
{
    int i;
    orte_app_context_t *app;

    for (i=0; i < jdat->apps->size; i++) {
        if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdat->apps, i))) {
            continue;
        }
        if (0 == strcmp(name, app->name)) {
            return app;
        }
    }

    /* get here if not found - create if requested */
    if (!create) {
        return NULL;
    }

    app = OBJ_NEW(orte_app_context_t);
    app->name = strdup(name);
    app->idx =jdat->num_apps;
    jdat->num_apps++;
    opal_pointer_array_set_item(jdat->apps, app->idx, app);
    return app;
}

static void copy_defaults(orte_job_t *target, orte_job_t *src)
{
    int i;
    orte_app_context_t *app;

    target->recovery_defined = src->recovery_defined;
    target->enable_recovery = src->enable_recovery;

    for (i=0; i < src->apps->size; i++) {
        if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(src->apps, i))) {
            continue;
        }
        OBJ_RETAIN(app);
        opal_pointer_array_set_item(target->apps, i, app);
        if (app->recovery_defined) {
            target->recovery_defined = true;
            if (0 < app->max_restarts ||
                -1 == app->max_restarts) {
                target->enable_recovery = true;
            }
        }
    }
    target->num_apps = src->num_apps;
    target->controls = src->controls;
    target->stdin_target = src->stdin_target;
    target->uid = src->uid;
    target->gid = src->gid;
}

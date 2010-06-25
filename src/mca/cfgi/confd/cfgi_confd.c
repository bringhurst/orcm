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
#include "opal/util/output.h"
#include "opal/util/argv.h"
#include "opal/util/opal_environ.h"
#include "opal/util/basename.h"

#include "orte/mca/plm/plm.h"
#include "orte/mca/plm/base/plm_private.h"
#include "orte/runtime/orte_globals.h"
#include "orte/util/context_fns.h"

#include "mca/cfgi/cfgi.h"

static opal_pointer_array_t installed_apps;
static orte_job_t *jdata;
static orte_app_context_t *app;

static orte_job_t *get_app(char *name, bool create);
static orte_app_context_t *get_exec(orte_job_t *jdat,
                                    char *name, bool create);
static void copy_defaults(orte_job_t *target, orte_job_t *src);

static bool check_job(orte_job_t *jdat);

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
    if (! qc_confd_init(cc, log_prefix, log_file, CONFD_TRACE))
        return FALSE;

    /*
     * wait for confd to allow subscriptions
     */
    if (! qc_wait_start(cc))
        return FALSE;

    /*
     * register a subscription to the install defaults
     */
    if (! qc_subscribe(cc,
                       QC_SUB_CONFIG_2PHASE,
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
                       QC_SUB_CONFIG_2PHASE,
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
     * register as a data provider
     */
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

    if (! qc_reg_data_cb(cc, &data_cbs))
        return FALSE;

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

    /*
     * retry the connection setup until it succeeds
     */
    while (! connect_to_confd(&cc, "orcm", stderr)) {
        sleep(1);
        qc_close(&cc);
    }

    /*
     * loop forever handling events, and reconnecting when needed
     */
    for (;;) {
        /*
         * handle events from confd (including the startup config events)
         * this returns only on error
         */
        qc_confd_poll(&cc);
        do {
            qc_close(&cc);
            sleep(1);
        } while (! qc_reconnect(&cc));
    }

    return NULL;
}


static int cfgi_confd_init(void)
{
    pthread_t confd_nanny_id;

    if (pthread_create(&confd_nanny_id,
		       NULL,            /* thread attributes */
		       confd_nanny,
		       NULL             /* thread parameter */) < 0) {
        opal_output(0, "Could not create confd nanny");

        return ORCM_ERROR;
    }

    OBJ_CONSTRUCT(&installed_apps, opal_pointer_array_t);
    opal_pointer_array_init(&installed_apps, 16, INT_MAX, 16);

    return ORCM_SUCCESS;
}

static int cfgi_confd_finalize(void)
{
  int i;
  orte_job_t *jd;

  for (i=0; i < installed_apps.size; i++) {
    if (NULL != (jd = (orte_job_t*)opal_pointer_array_get_item(&installed_apps, i))) {
      OBJ_RELEASE(jd);
    }
  }
    OBJ_DESTRUCT(&installed_apps);
    return ORCM_SUCCESS;
}


orcm_cfgi_base_module_t orcm_cfgi_confd_module = {
    cfgi_confd_init,
    cfgi_confd_finalize
};

static boolean cfg_handler(confd_hkeypath_t *kp,
                           enum cdb_iter_op  op,
                           confd_value_t    *value,
			   enum cdb_sub_notification notify_type,
                           long              which)
{
  return parse(kp, op, value, notify_type, false);
}


static boolean parse(confd_hkeypath_t *kp,
                     enum cdb_iter_op  op,
                     confd_value_t    *value,
	             enum cdb_sub_notification notify_type,
		     bool install)
{
    confd_value_t *clist, *vp;
    char *cptr, *param;
    unsigned int i, imax;
    int32_t i32;
    int j;
    orte_job_t *jdat;

    if (NULL == kp) {
        /* process the cmd */
        if (NULL != jdata) {
            opal_output(0, "event completed: %s",
                        install ? "INSTALL" : "RUN");
            if (CDB_SUB_PREPARE == notify_type) {
                opal_output(0, "NOTIFY: PREPARE");
                /* check to see if all required elements
                 * of the job object have been provided
                 */
                if (check_job(jdata)) {
                    opal_output(0, "PREPARE OKAY");
                    OBJ_RELEASE(jdata);
                    return TRUE;
                }
                /* nope - missing something, so notify failure */
                opal_output(0, "PREPARE FAILED");
                OBJ_RELEASE(jdata);
                return FALSE;
            } else if (CDB_SUB_ABORT == notify_type) {
                opal_output(0, "NOTIFY: ABORT - ignoring");
                OBJ_RELEASE(jdata);
                return TRUE;
            } else if (CDB_SUB_COMMIT == notify_type) {
                opal_output(0, "NOTIFY: COMMIT");
                if (install) {
                    /* add this to the installed data array */
                    opal_pointer_array_add(&installed_apps, jdata);
                    /* display the result */
                    opal_dss.dump(0, jdata, ORTE_JOB);
                    /* protect that data */
                    jdata = NULL;
                } else {
                    /*spawn this job */
                    if (NULL == jdata) {
                        opal_output(0, "ERROR: SPAWN A NULL JOB");
                    } else {
                        opal_output(0, "SPAWN %s", jdata->name);
                    /* display the result */
                    opal_dss.dump(0, jdata, ORTE_JOB);
#if 0
                        orte_plm.spawn(jdata);
#endif
                    }
                }
                return TRUE;
            } else {
                opal_output(0, "NOTIFY: UNKNOWN");
                return TRUE;
            }
        } else {
            opal_output(0, "event completed - null jdata");
        }
        return TRUE;
    }
    
    switch (op) {
    case MOP_CREATED:
        switch(CONFD_GET_XMLTAG(&kp->v[1][0])) {
        case orcm_app:
            /* new job */
            jdata = OBJ_NEW(orte_job_t);
            vp = qc_find_key(kp, orcm_app, 0);
            if (NULL == vp) {
                opal_output(0, "ERROR: BAD APP KEY");
                break;
            }
            jdata->name = strdup(CONFD_GET_CBUFPTR(vp));
            opal_output(0, "job: %s", jdata->name);
            break;
        case orcm_exec:
            /* new application */
            if (NULL == jdata) {
                opal_output(0, "NO ACTIVE JOB FOR VALUE SET");
                break;
            }
            app = OBJ_NEW(orte_app_context_t);
            vp = qc_find_key(kp, orcm_exec, 0);
            if (NULL == vp) {
                opal_output(0, "ERROR: BAD EXEC KEY");
                break;
            }
            app->name = strdup(CONFD_GET_CBUFPTR(vp));
            jdata->num_apps++;
            app->idx = opal_pointer_array_add(jdata->apps, app);
            break;
        case orcm_app_instance:
            /* run an instance of a possibly installed app */
            jdata = OBJ_NEW(orte_job_t);
            vp = qc_find_key(kp, orcm_app_instance, 0);
            if (NULL == vp) {
                opal_output(0, "ERROR: BAD APP-INSTANCE  KEY");
                break;
            }
            jdata->instance = strdup(CONFD_GET_CBUFPTR(vp));
            opal_output(0, "app-instance: %s", jdata->instance);
            break;
        default:
            opal_output(0, "BAD CREATE CMD");
            break;
        }
        break;
    case MOP_MODIFIED:
        /* modify a pre-existing object - could require creation */
        switch(CONFD_GET_XMLTAG(&kp->v[1][0])) {
        case orcm_app:
            opal_output(0, "MODIFY APP");
            break;
        case orcm_exec:
            opal_output(0, "MODIFY EXEC");
            break;
        case orcm_app_instance:
            opal_output(0, "MODIFY APP-INSTANCE");
            break;
        default:
            opal_output(0, "BAD MODIFY CMD");
            break;
        }
        break;
    case MOP_DELETED:
        opal_output(0, "MOP_DELETED NOT YET IMPLEMENTED");
        break;
    case MOP_VALUE_SET:
        switch(qc_get_xmltag(kp,1)) {
            /* JOB-LEVEL VALUES */
        case orcm_app_name:
            if (NULL == jdata->name) {
                jdata->name = strdup(CONFD_GET_CBUFPTR(value));
            }
            if (NULL != jdata->instance) {
                /* we are adding an instance of a known name
                 * see if this name is an installed app
                 */
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
                    opal_output(0, "COPYING DEFAULTS FOR APP %s TO INSTANCE %s", jdata->name, jdata->instance);
                    copy_defaults(jdat, jdata);
                    break;
                }
            }
            break;
        case orcm_gid:
            jdata->gid = CONFD_GET_INT32(value);
            break;
        case orcm_uid:
            jdata->uid = CONFD_GET_INT32(value);
            break;

            /* APP-LEVEL VALUES */
        case orcm_replicas:
            app->num_procs = CONFD_GET_INT32(value);
            break;
        case orcm_exec_name:
        case orcm_path:
            app->app = strdup(CONFD_GET_CBUFPTR(value));
            /* get the basename and install it as argv[0] */
            cptr = opal_basename(app->app);
            opal_argv_prepend_nosize(&app->argv, cptr);
            break;
        case orcm_local_max_restarts:
            app->max_local_restarts = CONFD_GET_INT32(value);
            break;
        case orcm_global_max_restarts:
            app->max_global_restarts = CONFD_GET_INT32(value);
            break;
        case orcm_leader_exclude:
            /* boolean - presence means do not allow
             * this app to become leader
             */
            opal_output(0, "ORCM_LDR_EXCLUDE");
            break;
        case orcm_version:
            opal_output(0, "version: %s", CONFD_GET_CBUFPTR(value));
            break;
        case orcm_config_set:
            cptr = CONFD_GET_CBUFPTR(value);
            param = mca_base_param_environ_variable("orcm", "confd", "config");
            opal_setenv(param, cptr, true, &app->env);
            free(param);
            break;
        case orcm_names:
            /* the equivalent to -host */
            clist = CONFD_GET_LIST(value);
            imax = CONFD_GET_LISTSIZE(value);
            for (i=0; i < imax; i++) {
                opal_argv_append_nosize(&app->dash_host, CONFD_GET_CBUFPTR(&clist[i]));
            }
            break;
        case orcm_argv:
            clist = CONFD_GET_LIST(value);
            imax = CONFD_GET_LISTSIZE(value);
            for (i=0; i < imax; i++) {
                opal_argv_append_nosize(&app->argv, CONFD_GET_CBUFPTR(&clist[i]));
            }
            break;
        case orcm_env:
            clist = CONFD_GET_LIST(value);
            imax = CONFD_GET_LISTSIZE(value);
            for (i=0; i < imax; i++) {
                opal_argv_append_nosize(&app->env, CONFD_GET_CBUFPTR(&clist[i]));
            }
            break;
        case orcm_nodes:
            i32 = CONFD_GET_ENUM_HASH(value);
            /* the value is a flag indicating the class
             * of hosts this app is restricted to operate
             * on - need to add a field in app_context_t
             * to flag that info
             */
            break;
        case orcm_working_dir:
            app->cwd = strdup(CONFD_GET_CBUFPTR(value));
            break;

            /* IGNORED VALUES */
        case orcm_name:
            /* dealt with in the create when this key
             * was provided - so ignore it here
             */
            break;
        case orcm_instance_name:
            /* we already have this info - it was the key
             * when we got the create command, so just
             * keep what we already have
             */
            break;

        default:
            opal_output(0, "NON-UNDERSTOOD XML TAG");
            break;
        }
        break;
    default:
        opal_output(0, "WHAT THE HECK?");
        break;
    }
    return TRUE;
}

static boolean install_handler(confd_hkeypath_t *kp,
                               enum cdb_iter_op  op,
                               confd_value_t    *value,
			       enum cdb_sub_notification notify_type,
                               long              which)
{
  return parse(kp, op, value, notify_type, true);
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

    /*
     * look at the first XML tag in the keypath to see which element
     * is being requested
     */
    switch (qc_get_xmltag(kp, 1)) {
    case orcm_job_id:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            goto notfound;
        }
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        if (NULL == jdat) {
            /* job no longer exists */
            goto notfound;
        }
        CONFD_SET_UINT32(&val, jdat->jobid);
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    case orcm_job_name:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            goto notfound;
        }
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        if (NULL == jdat) {
            /* job no longer exists */
            goto notfound;
        }
        if (NULL == jdat->name) {
            CONFD_SET_CBUF(&val, "NONE", 4);
        } else {
            CONFD_SET_CBUF(&val, jdat->name, strlen(jdat->name));
        }
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;
        break;

    case orcm_job_state:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            goto notfound;
        }
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_XMLTAG(vp));
        if (NULL == jdat) {
            /* job no longer exists */
            goto notfound;
        }
        /* sadly, have to translate job state into yang's enum values */
        switch (jdat->state) {
        case ORTE_JOB_STATE_UNDEF:
            ret = orcm_PENDING;
            break;
        case ORTE_JOB_STATE_INIT:
            ret = orcm_INIT;
            break;
        case ORTE_JOB_STATE_RESTART:
            ret = orcm_RESTART;
            break;
        case ORTE_JOB_STATE_LAUNCHED:
            ret = orcm_LAUNCHED;
            break;
        case ORTE_JOB_STATE_RUNNING:
            ret = orcm_RUNNING;
            break;
        case ORTE_JOB_STATE_UNTERMINATED:
            ret = orcm_UNTERMINATED;
            break;
        case ORTE_JOB_STATE_TERMINATED:
            ret = orcm_TERMINATED;
            break;
        case ORTE_JOB_STATE_ABORTED:
            ret = orcm_ABORTED;
            break;
        case ORTE_JOB_STATE_FAILED_TO_START:
            ret = orcm_FAILED_TO_START;
            break;
        case ORTE_JOB_STATE_ABORTED_BY_SIG:
            ret = orcm_ABORTED_BY_SIG;
            break;
        case ORTE_JOB_STATE_ABORTED_WO_SYNC:
            ret = orcm_ABORTED_WO_SYNC;
            break;
        case ORTE_JOB_STATE_KILLED_BY_CMD:
            ret = orcm_KILLED_BY_CMD;
            break;
        case ORTE_JOB_STATE_NEVER_LAUNCHED:
            ret = orcm_NEVER_LAUNCHED;
            break;
        case ORTE_JOB_STATE_ABORT_ORDERED:
            ret = orcm_ABORT_ORDERED;
            break;
        default:
            goto notfound;
        }
        CONFD_SET_ENUM_HASH(&val, ret);
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    case orcm_num_replicas_requested:
    case orcm_num_replicas_launched:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            goto notfound;
        }
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        if (NULL == jdat) {
            /* job no longer exists */
            goto notfound;
        }
        ui16 = jdat->num_procs;
        CONFD_SET_UINT16(&val, ui16);
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    case orcm_num_replicas_terminated:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            goto notfound;
        }
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        if (NULL == jdat) {
            /* job no longer exists */
            goto notfound;
        }
        ui16 = jdat->num_terminated;
        CONFD_SET_UINT16(&val, ui16);
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    case orcm_aborted:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            goto notfound;
        }
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        if (NULL == jdat) {
            /* job no longer exists */
            goto notfound;
        }
        break;

    case orcm_aborted_procedure:
        goto notfound;
        break;

    case orcm_app_context:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            goto notfound;
        }
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        if (NULL == jdat) {
            /* job no longer exists */
            goto notfound;
        }
        goto notfound;
        break;

    case orcm_path:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "JOB ID NOT FOUND");
            goto notfound;
        }
        /* get the referenced app_context */
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        if (NULL == jdat) {
            /* job no longer exists */
            goto notfound;
        }
        vp = qc_find_key(kp, orcm_app_context, 0);
        if (NULL == vp) {
            opal_output(0, "APP IDX NOT FOUND");
            goto notfound;
        }
        app = (orte_app_context_t*)opal_pointer_array_get_item(jdat->apps, CONFD_GET_INT32(vp));
        if (NULL == app) {
            opal_output(0, "APP NOT FOUND");
            goto notfound;
        }
        if (NULL == app->name) {
            CONFD_SET_CBUF(&val, "NONE", 4);
        } else {
            CONFD_SET_CBUF(&val, app->name, strlen(app->name));
        }
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;
        break;

    case orcm_max_local_restarts:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "JOB ID NOT FOUND");
            goto notfound;
        }
        /* get the referenced app_context */
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        if (NULL == jdat) {
            opal_output(0, "JOB %d NOT FOUND", CONFD_GET_UINT32(vp));
            /* job no longer exists */
            goto notfound;
        }
        vp = qc_find_key(kp, orcm_app_context, 0);
        if (NULL == vp) {
            opal_output(0, "APP IDX NOT FOUND");
            goto notfound;
        }
        app = (orte_app_context_t*)opal_pointer_array_get_item(jdat->apps, CONFD_GET_INT32(vp));
        if (NULL == app) {
            opal_output(0, "APP %d NOT FOUND", CONFD_GET_INT32(vp));
            goto notfound;
        }
        i32 = app->max_local_restarts;
        CONFD_SET_INT32(&val, i32);
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;
        break;

    case orcm_max_global_restarts:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "JOB ID NOT FOUND");
            goto notfound;
        }
        /* get the referenced app_context */
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        if (NULL == jdat) {
            /* job no longer exists */
            goto notfound;
        }
        vp = qc_find_key(kp, orcm_app_context, 0);
        if (NULL == vp) {
            opal_output(0, "APP IDX NOT FOUND");
            goto notfound;
        }
        app = (orte_app_context_t*)opal_pointer_array_get_item(jdat->apps, CONFD_GET_INT32(vp));
        if (NULL == app) {
            opal_output(0, "APP NOT FOUND");
            goto notfound;
        }
        i32 = app->max_global_restarts;
        CONFD_SET_INT32(&val, i32);
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;
        break;

    case orcm_replica:
        opal_output(0, "REPLICA");
        goto notfound;
        break;

    case orcm_app_name:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "JOB ID NOT FOUND");
            goto notfound;
        }
        /* get the referenced app_context */
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        if (NULL == jdat) {
            /* job no longer exists */
            goto notfound;
        }
        vp = qc_find_key(kp, orcm_app_context, 0);
        if (NULL == vp) {
            opal_output(0, "APP IDX NOT FOUND");
            goto notfound;
        }
        app = (orte_app_context_t*)opal_pointer_array_get_item(jdat->apps, CONFD_GET_INT32(vp));
        if (NULL == app) {
            opal_output(0, "APP NOT FOUND");
            goto notfound;
        }
        if (NULL == app->name) {
            CONFD_SET_CBUF(&val, "UNNAMED", strlen("UNNAMED"));
        } else {
            CONFD_SET_CBUF(&val, app->name, strlen(app->name));
        }
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    case orcm_pid:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "JOB ID NOT FOUND");
            goto notfound;
        }
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        if (NULL == jdat) {
            opal_output(0, "JOB %d NOT FOUND", CONFD_GET_UINT32(vp));
            /* job no longer exists */
            goto notfound;
        }
        vp = qc_find_key(kp, orcm_replica, 0);
        if (NULL == vp) {
            opal_output(0, "PROC ID NOT FOUND");
            goto notfound;
        }
        proc = (orte_proc_t*)opal_pointer_array_get_item(jdat->procs, CONFD_GET_INT32(vp));
        if (NULL == proc) {
            opal_output(0, "PROC %s NOT FOUND", ORTE_VPID_PRINT(CONFD_GET_INT32(vp)));
        }
        CONFD_SET_UINT32(&val, proc->pid);
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    case orcm_exit_code:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "JOB ID NOT FOUND");
            goto notfound;
        }
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        if (NULL == jdat) {
            opal_output(0, "JOB %d NOT FOUND", CONFD_GET_UINT32(vp));
            /* job no longer exists */
            goto notfound;
        }
        vp = qc_find_key(kp, orcm_replica, 0);
        if (NULL == vp) {
            opal_output(0, "PROC ID NOT FOUND");
            goto notfound;
        }
        proc = (orte_proc_t*)opal_pointer_array_get_item(jdat->procs, CONFD_GET_INT32(vp));
        if (NULL == proc) {
            opal_output(0, "PROC %s NOT FOUND", ORTE_VPID_PRINT(CONFD_GET_INT32(vp)));
        }
        CONFD_SET_INT32(&val, proc->exit_code);
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    case orcm_app_context_id:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "JOB ID NOT FOUND");
            goto notfound;
        }
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        if (NULL == jdat) {
            opal_output(0, "JOB %d NOT FOUND", CONFD_GET_UINT32(vp));
            /* job no longer exists */
            goto notfound;
        }
        vp = qc_find_key(kp, orcm_replica, 0);
        if (NULL == vp) {
            opal_output(0, "PROC ID NOT FOUND");
            goto notfound;
        }
        proc = (orte_proc_t*)opal_pointer_array_get_item(jdat->procs, CONFD_GET_INT32(vp));
        if (NULL == proc) {
            opal_output(0, "PROC %s NOT FOUND", ORTE_VPID_PRINT(CONFD_GET_INT32(vp)));
        }
        CONFD_SET_UINT32(&val, proc->app_idx);
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    case orcm_rml_contact_info:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "JOB ID NOT FOUND");
            goto notfound;
        }
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        if (NULL == jdat) {
            opal_output(0, "JOB %d NOT FOUND", CONFD_GET_UINT32(vp));
            /* job no longer exists */
            goto notfound;
        }
        vp = qc_find_key(kp, orcm_replica, 0);
        if (NULL == vp) {
            opal_output(0, "PROC ID NOT FOUND");
            goto notfound;
        }
        proc = (orte_proc_t*)opal_pointer_array_get_item(jdat->procs, CONFD_GET_INT32(vp));
        if (NULL == proc) {
            opal_output(0, "PROC %s NOT FOUND", ORTE_VPID_PRINT(CONFD_GET_INT32(vp)));
        }
        if (NULL == proc->rml_uri) {
            CONFD_SET_CBUF(&val, "NULL", strlen("NULL"));
        } else {
            CONFD_SET_CBUF(&val, "SET", strlen("SET"));
        }
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    case orcm_replica_state:
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "JOB ID NOT FOUND");
            goto notfound;
        }
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        if (NULL == jdat) {
            opal_output(0, "JOB %d NOT FOUND", CONFD_GET_UINT32(vp));
            /* job no longer exists */
            goto notfound;
        }
        vp = qc_find_key(kp, orcm_replica, 0);
        if (NULL == vp) {
            opal_output(0, "PROC ID NOT FOUND");
            goto notfound;
        }
        proc = (orte_proc_t*)opal_pointer_array_get_item(jdat->procs, CONFD_GET_INT32(vp));
        if (NULL == proc) {
            opal_output(0, "PROC %s NOT FOUND", ORTE_VPID_PRINT(CONFD_GET_INT32(vp)));
        }
        /* sadly, have to translate proc state into yang's enum values */
        switch (proc->state) {
        case ORTE_PROC_STATE_UNDEF:
            ret = orcm_PENDING;
            break;
        case ORTE_PROC_STATE_INIT:
            ret = orcm_INIT;
            break;
        case ORTE_PROC_STATE_RESTART:
            ret = orcm_RESTART;
            break;
        case ORTE_PROC_STATE_LAUNCHED:
            ret = orcm_LAUNCHED;
            break;
        case ORTE_PROC_STATE_RUNNING:
            ret = orcm_RUNNING;
            break;
        case ORTE_PROC_STATE_REGISTERED:
            ret = orcm_REGISTERED;
            break;
        case ORTE_PROC_STATE_UNTERMINATED:
            ret = orcm_UNTERMINATED;
            break;
        case ORTE_PROC_STATE_TERMINATED:
            ret = orcm_TERMINATED;
            break;
        case ORTE_PROC_STATE_KILLED_BY_CMD:
            ret = orcm_KILLED_BY_CMD;
            break;
        case ORTE_PROC_STATE_ABORTED:
            ret = orcm_ABORTED;
            break;
        case ORTE_PROC_STATE_FAILED_TO_START:
            ret = orcm_FAILED_TO_START;
            break;
        case ORTE_PROC_STATE_ABORTED_BY_SIG:
            ret = orcm_ABORTED_BY_SIG;
            break;
        case ORTE_PROC_STATE_TERM_WO_SYNC:
            ret = orcm_TERM_WO_SYNC;
            break;
        case ORTE_PROC_STATE_COMM_FAILED:
            ret = orcm_COMM_FAILED;
            break;
        case ORTE_PROC_STATE_SENSOR_BOUND_EXCEEDED:
            ret = orcm_SENSOR_BOUND_EXCEEDED;
            break;
        case ORTE_PROC_STATE_CALLED_ABORT:
            ret = orcm_CALLED_ABORT;
            break;
        case ORTE_PROC_STATE_HEARTBEAT_FAILED:
            ret = orcm_HEARTBEAT_FAILED;
            break;
        default:
            goto notfound;
        }
        CONFD_SET_ENUM_HASH(&val, ret);
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    case orcm_node_id:
        opal_output(0, "NODE");
        break;

    case orcm_node_name:
        vp = qc_find_key(kp, orcm_orte_node, 0);
        if (NULL == vp) {
            opal_output(0, "NODE ID NOT FOUND");
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_INT32(vp)))) {
            opal_output(0, "NODE %d NOT FOUND", CONFD_GET_INT32(vp));
            goto notfound;
        }
        CONFD_SET_CBUF(&val, node->name, strlen(node->name));
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    case orcm_heartbeat_seconds:
        vp = qc_find_key(kp, orcm_orte_node, 0);
        if (NULL == vp) {
            opal_output(0, "NODE ID NOT FOUND");
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_INT32(vp)))) {
            opal_output(0, "NODE %d NOT FOUND", CONFD_GET_INT32(vp));
            goto notfound;
        }
        CONFD_SET_UINT8(&val, 1);
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    case orcm_state:
        vp = qc_find_key(kp, orcm_orte_node, 0);
        if (NULL == vp) {
            opal_output(0, "NODE ID NOT FOUND");
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_INT32(vp)))) {
            opal_output(0, "NODE %d NOT FOUND", CONFD_GET_INT32(vp));
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
        return CONFD_OK;

    case orcm_daemon_id:
        vp = qc_find_key(kp, orcm_orte_node, 0);
        if (NULL == vp) {
            opal_output(0, "NODE ID NOT FOUND");
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_INT32(vp)))) {
            opal_output(0, "NODE %d NOT FOUND", CONFD_GET_INT32(vp));
            goto notfound;
        }
        CONFD_SET_CBUF(&val, ORTE_VPID_PRINT(node->daemon->name.vpid),
                       strlen(ORTE_VPID_PRINT(node->daemon->name.vpid)));
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    case orcm_temperature:
        vp = qc_find_key(kp, orcm_orte_node, 0);
        if (NULL == vp) {
            opal_output(0, "NODE ID NOT FOUND");
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_INT32(vp)))) {
            opal_output(0, "NODE %d NOT FOUND", CONFD_GET_INT32(vp));
            goto notfound;
        }
        CONFD_SET_INT8(&val, 1);
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    case orcm_num_procs:
        vp = qc_find_key(kp, orcm_orte_node, 0);
        if (NULL == vp) {
            opal_output(0, "NODE ID NOT FOUND");
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_INT32(vp)))) {
            opal_output(0, "NODE %d NOT FOUND", CONFD_GET_INT32(vp));
            goto notfound;
        }
        CONFD_SET_INT32(&val, node->num_procs);
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    default:
        goto notfound;
    }

 notfound:
    confd_data_reply_not_found(tctx);

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
    int i;
    orte_job_t *jdat;
    orte_proc_t *p;
    int32_t app_idx;

    switch (qc_get_xmltag(kp, 1)) {
    case orcm_job:
        if (next == -1) {
            next = 0;
        }
	/* look for next non-NULL job site */
	for (i=next; i < orte_job_data->size; i++) {
            if (NULL != opal_pointer_array_get_item(orte_job_data, i)) {
                CONFD_SET_UINT32(&key, i);
                confd_data_reply_next_key(tctx, &key, 1, i + 1);
                return CONFD_OK;
            }
	}
        goto notfound;
        break;

    case orcm_app_context:
        ky = qc_find_key(kp, orcm_job, 0);
        if (NULL == ky) {
            opal_output(0, "APP_CTX KEY NOT FOUND");
            goto notfound;
        }
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_INT32(ky));
        if (NULL == jdat) {
            goto notfound;
        }
        if (next == -1) {
            next = 0;
        }
        /* look for next non-NULL app context */
	for (i=next; i < jdat->apps->size; i++) {
            if (NULL != opal_pointer_array_get_item(jdat->apps, i)) {
                CONFD_SET_INT32(&key, i);
                confd_data_reply_next_key(tctx, &key, 1, i + 1);
                return CONFD_OK;
            }
	}
        goto notfound;
        break;

    case orcm_replica:
        ky = qc_find_key(kp, orcm_job, 0);
        if (NULL == ky) {
            opal_output(0, "JOB KEY NOT FOUND FOR REPLICA");
            goto notfound;
        }
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(ky));
        if (NULL == jdat) {
            opal_output(0, "JOB NOT FOUND");
            goto notfound;
        }
        ky = qc_find_key(kp, orcm_app_context, 0);
        if (NULL == ky) {
            opal_output(0, "APP KEY NOT FOUND FOR REPLICA");
            goto notfound;
        }
        app_idx = CONFD_GET_INT32(ky);
        if (next == -1) {
            next = 0;
        }
        /* look for next non-NULL proc for this app */
	for (i=next; i < jdat->procs->size; i++) {
            if (NULL != (p = (orte_proc_t*)opal_pointer_array_get_item(jdat->procs, i)) &&
                p->app_idx == app_idx) {
                CONFD_SET_UINT32(&key, i);
                confd_data_reply_next_key(tctx, &key, 1, i + 1);
                return CONFD_OK;
            }
	}
        goto notfound;
        break;

    case orcm_orte_node:
        /* look for next non-NULL node */
        if (next == -1) {
            next = 0;
        }
        for (i=next; i < orte_node_pool->size; i++) {
            if (NULL != opal_pointer_array_get_item(orte_node_pool, i)) {
                CONFD_SET_INT32(&key, i);
                confd_data_reply_next_key(tctx, &key, 1, i+1);
                return CONFD_OK;
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
    app->idx = opal_pointer_array_add(jdat->apps, app);
    return app;
}

static void copy_defaults(orte_job_t *target, orte_job_t *src)
{
    int i;
    orte_app_context_t *app;

    for (i=0; i < src->apps->size; i++) {
        if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(src->apps, i))) {
            continue;
        }
        OBJ_RETAIN(app);
        opal_pointer_array_set_item(target->apps, i, app);
    }
    target->num_apps = src->num_apps;
    target->controls = src->controls;
    target->stdin_target = src->stdin_target;
    target->uid = src->uid;
    target->gid = src->gid;
}

static bool check_job(orte_job_t *jdat)
{
    int i;
    orte_app_context_t *app;

    /* must have at least one app */
    if (NULL == opal_pointer_array_get_item(jdat->apps, 0)) {
        opal_output(0, "JOB %s MISSING APP", (NULL == jdat->name) ? "UNNAMED" : jdat->name);
        return false;
    }
    if (jdat->num_apps <= 0) {
        opal_output(0, "JOB %s ZERO APPS", (NULL == jdat->name) ? "UNNAMED" : jdat->name);
        return false;
    }
    /* we require that an executable and the number of procs be specified
     * for each app
     */
    for (i=0; i < jdat->apps->size; i++) {
        if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdat->apps, i))) {
            continue;
        }
        if (NULL == app->app || NULL == app->argv || 0 == opal_argv_count(app->argv)) {
            opal_output(0, "JOB %s NO EXE FOR APP %d", (NULL == jdat->name) ? "UNNAMED" : jdat->name, i);
            return false;
        }
        if (app->num_procs <= 0) {
            opal_output(0, "JOB %s ZERO PROCS FOR APP %d", (NULL == jdat->name) ? "UNNAMED" : jdat->name, i);
            return false;
        }
        /* ensure we can find the executable */
        if (NULL == app->env || 0 == opal_argv_count(app->env)) {
            if (ORTE_SUCCESS != orte_util_check_context_app(app, environ)) {
                opal_output(0, "JOB %s EXE NOT FOUND FOR APP %d IN STD ENVIRON", (NULL == jdat->name) ? "UNNAMED" : jdat->name, i);
                return false;
            }
        } else {
            if (ORTE_SUCCESS != orte_util_check_context_app(app, app->env)) {
                opal_output(0, "JOB %s EXE NOT FOUND FOR APP %d", (NULL == jdat->name) ? "UNNAMED" : jdat->name, i);
                return false;
            }
        }
    }
    return true;
}

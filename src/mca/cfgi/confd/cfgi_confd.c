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

#include "orte/runtime/orte_globals.h"

#include "mca/cfgi/cfgi.h"

static opal_pointer_array_t installed_apps;

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

static orte_job_t *jdata=NULL;
static orte_app_context_t *app=NULL;

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
    confd_value_t *clist;
    char *cptr, *param;
    unsigned int i, imax;
    int32_t i32;

    if (NULL == kp) {
        /* process the cmd */
        if (NULL != jdata) {
	  opal_output(0, "event completed: %s",
		      install ? "INSTALL" : "RUN");
            if (CDB_SUB_PREPARE == notify_type) {
	      opal_output(0, "NOTIFY: PREPARE");
	      /* run the job through plm_job_setup, but
	       * don't launch it - return the result as
	       * this indicates that we would, or would
	       * not, be able to launch as requested
	       */
              OBJ_RELEASE(jdata);
	      return TRUE;
            } else if (CDB_SUB_ABORT == notify_type) {
              opal_output(0, "NOTIFY: ABORT - ignoring");
            } else if (CDB_SUB_COMMIT == notify_type) {
              opal_output(0, "NOTIFY: COMMIT");
              opal_dss.dump(0, jdata, ORTE_JOB);
            } else {
              opal_output(0, "NOTIFY: UNKNOWN");
            }
        } else {
            opal_output(0, "event completed - null jdata");
        }
	if (NULL !=  jdata) {
	  OBJ_RELEASE(jdata);
	}
        return TRUE;
    }
    
    switch (op) {
        case MOP_CREATED:
            switch(CONFD_GET_XMLTAG(&kp->v[1][0])) {
                case orcm_app:
                    /* new job */
                    jdata = OBJ_NEW(orte_job_t);
                    jdata->name = strdup(CONFD_GET_CBUFPTR(&kp->v[0][0]));
                    opal_output(0, "job: %s", jdata->name);
                    break;
                case orcm_exec:
                    /* new application */
                    if (NULL == jdata) {
                        opal_output(0, "NO ACTIVE JOB FOR VALUE SET");
                        break;
                    }
                    app = OBJ_NEW(orte_app_context_t);
		    app->name = strdup(CONFD_GET_CBUFPTR(&kp->v[0][0]));
                    jdata->num_apps++;
                    opal_pointer_array_add(jdata->apps, app);
                    break;
                default:
                    opal_output(0, "BAD CREATE CMD");
                    break;
            }
            break;
        case MOP_DELETED:
            opal_output(0, "MOP_DELETED NOT YET IMPLEMENTED");
            break;
        case MOP_VALUE_SET:
            switch(CONFD_GET_XMLTAG(&kp->v[0][0])) {
                case orcm_replicas:
                    app->num_procs = CONFD_GET_INT32(value);
                    break;
                case orcm_exec_name:
                case orcm_path:
		    app->app = strdup(CONFD_GET_CBUFPTR(value));
		    /* get the basename and install it as argv[0] */
		    cptr =opal_basename(app->app);
#if 0
		    opal_argv_prepend_nosize(&app->argv, cptr);
#endif
                    break;
                case orcm_local_max_restarts:
                    app->max_local_restarts = CONFD_GET_INT32(value);
                    break;
                case orcm_global_max_restarts:
                    app->max_global_restarts = CONFD_GET_INT32(value);
                    break;
                case orcm_instance_name:
		  /* we already have this info - it was the key
		   * when we got the create command, so just
		   * keep what we already have
		   */
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
                case orcm_name:
		  /* dealt with in the create when this key
		   * was provided - so ignore it here
		   */
                    break;
                case orcm_working_dir:
                    app->cwd = strdup(CONFD_GET_CBUFPTR(value));
                    break;
                case orcm_gid:
                    jdata->gid = CONFD_GET_INT32(value);
                    break;
                case orcm_uid:
                    jdata->uid = CONFD_GET_INT32(value);
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
    int index, arr_ix;
    char *cp;
    confd_value_t val, *vp;

    /*
     * look at the first XML tag in the keypath to see which element
     * is being requested
     */
    switch (qc_get_xmltag(kp, 1)) {
    case orcm_job_state:
        CONFD_SET_ENUM_HASH(&val, 1);
        confd_data_reply_value(tctx, &val);
        return CONFD_OK;

    case orcm_job_id:
      CONFD_SET_UINT32(&val, 123);
      break;

    default:
      confd_data_reply_not_found(tctx);
    }

    return CONFD_OK;
}


/*
 * compute the key to follow 'next'
 */
static int orcm_get_next (struct confd_trans_ctx *tctx,
			  confd_hkeypath_t       *kp,
			  long next)
{
    confd_value_t key;
    int i;

    switch (qc_get_xmltag(kp, 1)) {
    case orcm_job:                  // handle the table
        if (next == -1) {
            next = 0;                   // first entry has index 1
        }
	/* look for next non-NULL job site */
	for (i=next; i < orte_job_data->size; i++) {
	  if (NULL != opal_pointer_array_get_item(orte_job_data, i)) {
              CONFD_SET_UINT32(&key, i);
              confd_data_reply_next_key(tctx, &key, 1, i + 1);
              return CONFD_OK;
	  }
	}
        confd_data_reply_next_key(tctx, NULL, -1, -1);
        return CONFD_OK;
        break;
    }

    /*
     * not found
     */
    confd_data_reply_next_key(tctx, NULL, -1, -1);

    return CONFD_OK;
}


#if 0
    container main {
        config false;
        tailf:callpoint "orcm-oper";

        leaf pid {
            type int16;
        }

        list environ {
            key "index";
            leaf index {
                type uint16;
            }
            leaf name {
                type string;
            }
            leaf value {
                type string;
            }
        }
    }
#endif

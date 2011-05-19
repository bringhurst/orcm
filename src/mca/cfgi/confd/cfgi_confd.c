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

#if WANT_LOCAL_CONFD
#include "orcm_q_confd.h"
#else
#include <q_confd.h>
#endif

#include "orcm-confd.h"

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
#include "orte/util/error_strings.h"

#include "mca/pnp/pnp.h"
#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/base/public.h"
#include "mca/cfgi/base/private.h"
#include "cfgi_confd.h"


/* API */
static int cfgi_confd_init(void);
static int cfgi_confd_finalize(void);
static void cfgi_confd_activate(void);

orcm_cfgi_base_module_t orcm_cfgi_confd_module = {
    cfgi_confd_init,
    cfgi_confd_finalize,
    cfgi_confd_activate
};

/* Local storage */
static opal_pointer_array_t active_apps;
static bool thread_active=false;
static bool initialized = false;
static bool enabled = false;
static pthread_t confd_nanny_id;
static char **interfaces=NULL;
static bool confd_master_is_local=false;

/* Local fns */
static orte_job_t *get_app(char *name, bool create);

static boolean parse(confd_hkeypath_t *kp,
			   enum cdb_iter_op  op,
			   confd_value_t    *value,
			   enum cdb_sub_notification notify_type,
			   long              which);

static cmdtbl_t config_cmds[] = {
  { { orcm_orcmRun  }, parse, },
  { { qc_eod_notify }, parse, },
  { },
};

static int orcm_get_elem(struct confd_trans_ctx *tctx,
                         confd_hkeypath_t       *kp);

static int orcm_get_next(struct confd_trans_ctx *tctx,
                         confd_hkeypath_t       *kp,
                         long next);

static boolean orcm_clear(int maapisock,
                          struct confd_user_info *uinfo,
                          int argc, char **argv, long which);

static boolean orcm_clear_completion(struct confd_user_info *uinfo,
                                     int                     cli_style,
                                     char                   *token,
                                     int                     comp_char,
                                     confd_hkeypath_t       *kp,
                                     char                   *cmdpath,
                                     char                   *param_id);

static boolean show_installed_sw(int maapisock,
                                 struct confd_user_info *uinfo,
                                 int argc, char **argv, long which);

static boolean show_configured_sw(int maapisock,
                                  struct confd_user_info *uinfo,
                                  int argc, char **argv, long which);

static boolean run_config_completion(struct confd_user_info *uinfo,
                                     int                     cli_style,
                                     char                   *token,
                                     int                     comp_char,
                                     confd_hkeypath_t       *kp,
                                     char                   *cmdpath,
                                     char                   *param_id);

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
     * register a subscription
     */
    if (! qc_subscribe(cc,
                       QC_SUB_CONFIG,
                       QC_SUB_EOD_NOTIFY,       /* flags */
                       5,			/* priority */
                       orcm__ns,
                       config_cmds,
                       "/orcmRun"))
        return FALSE;

    /*
     * tell confd we're done with subscriptions
     */
    if (! qc_subscribe_done(cc))
        return FALSE;

    /*
     * register CLI command handlers
     */
    if (! qc_reg_cmdpoint(cc, "clear", orcm_clear, 0))
        return FALSE;

    if (! qc_reg_cmdpoint(cc, "show_installed_apps", show_installed_sw, 0))
        return FALSE;

    if (! qc_reg_cmdpoint(cc, "show_configured_apps", show_configured_sw, 0))
        return FALSE;

    /*
     * register completion handler for the command */
    if (! qc_reg_completion(cc, "orcm-clear-completion", orcm_clear_completion))
        return FALSE;

    /* register completion handler for configuring running apps */
    if (! qc_reg_completion(cc, "orcm-run-config-completion", run_config_completion))
        return FALSE;

    /*
     *register as a data provider
     */
    if (!qc_reg_data_cb(cc, &data_cbs)) {
        return FALSE;
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
    int cnt;
    char log_pfx[48];

    thread_active = true;

    snprintf(log_pfx, sizeof(log_pfx), "ORCM-DVM/%d", \
             (int)ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid));

    cnt = 0;
    /* retry the connection setup infinite times */
    while (! connect_to_confd(&cc, log_pfx, stderr)) {
        cnt++;
        if (10 == cnt) {
            opal_output(0, "%s FAILED TO CONNECT TO CONFD",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            cnt = 0;
        }
        sleep(1);
        qc_close(&cc);
    }

    if (FALSE == cfgi_confd_subscribe(&cc)) {
        opal_output(0, "%s FAILED TO SUBSCRIBE TO CONFD",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        thread_active = false;
        return NULL;
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
    /* if we already did this, don't do it again */
    if (initialized) {
        return ORCM_SUCCESS;
    }

    OBJ_CONSTRUCT(&active_apps, opal_pointer_array_t);
    opal_pointer_array_init(&active_apps, 16, INT_MAX, 16);

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

    /* make the connection to confd*/
    if (pthread_create(&confd_nanny_id,
                       NULL,            /* thread attributes */
                       confd_nanny,
                       NULL             /* thread parameter */) < 0) {
        /* if we can't start a thread, then just return */
        return;
    }
    /* don't wait for the connection as this can cause us
     * to hang here. Let the thread just cycle while it
     * looks for confd
     */

    enabled = true;
}

static int cfgi_confd_finalize(void)
{
    int i;
    orte_job_t *jd;

    if (initialized) {

        for (i=0; i < active_apps.size; i++) {
            if (NULL != (jd = (orte_job_t*)opal_pointer_array_get_item(&active_apps, i))) {
                OBJ_RELEASE(jd);
            }
        }
        OBJ_DESTRUCT(&active_apps);

        opal_argv_free(interfaces);

        initialized = false;
    }
    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output, "cfgi:confd finalized"));
    return ORCM_SUCCESS;
}


/* find a given application in the installed config */
static bool find_app(confd_hkeypath_t *kp,
                     orcm_cfgi_app_t **app,
                     char **application)
{
    confd_value_t *vp;
    orcm_cfgi_app_t *aptr;
    char *cptr;
    int j;

    *app = NULL;
    *application = strdup("foo");
    vp = qc_find_key(kp, orcm_app, 0);
    if (NULL == vp) {
        return false;
    }
    cptr = CONFD_GET_CBUFPTR(vp);
    *application = strdup(cptr);
    for (j=0; j < orcm_cfgi_base.installed_apps.size; j++) {
        if (NULL == (aptr = (orcm_cfgi_app_t*)opal_pointer_array_get_item(&orcm_cfgi_base.installed_apps, j))) {
            continue;
        }
        if (0 == strcmp(cptr, aptr->application)) {
            OPAL_OUTPUT_VERBOSE((3, orcm_cfgi_base.output,
                                 "FOUND EXISTING INSTALLED APP %s", cptr));
            *app = aptr;
            return true;
        }
    }
    /* didn't find the app - but we still want to store the configuration */
    OPAL_OUTPUT_VERBOSE((3, orcm_cfgi_base.output,
                         "APP %s NOT INSTALLED", *application));
    return true;
}

/* find a given version in the installed config */
static bool find_version(confd_hkeypath_t *kp,
                         orcm_cfgi_exec_t **exe,
                         orcm_cfgi_version_t **vers)
{
    orcm_cfgi_app_t *app;
    orcm_cfgi_exec_t *exec, *eptr;
    orcm_cfgi_version_t *vptr;
    confd_value_t *vp, *vt;
    char *cptr, *ctmp, *application;
    int j;

    /* set default */
    *exe = NULL;
    *vers = NULL;

    if (!find_app(kp, &app, &application)) {
        /* no key found */
        return false;
    }
    vp = qc_find_key(kp, orcm_exec, 0);
    if (NULL == vp) {
        /* no exec key found */
        return false;
    }
    cptr = CONFD_GET_CBUFPTR(vp);
    /* find the version */
    vt = qc_find_key(kp, orcm_version, 0);
    if (NULL == vt) {
        return false;
    }
    ctmp = CONFD_GET_CBUFPTR(vt);
    if (NULL == app) {
        /* didn't find the app - but allow to continue
         * as we might just be storing config for an
         * uninstalled app
         */
        return true;
    }
    /* find the executable */
    exec = NULL;
    for (j=0; j < app->executables.size; j++) {
        if (NULL == (eptr = (orcm_cfgi_exec_t*)opal_pointer_array_get_item(&app->executables, j))) {
            continue;
        }
        if (0 == strcmp(eptr->appname, cptr)) {
            OPAL_OUTPUT_VERBOSE((3, orcm_cfgi_base.output,
                                 "FOUND EXISTING EXECUTABLE %s IN APP %s",
                                 cptr, app->application));
            exec = eptr;
            break;
        }
    }
    if (NULL == exec) {
        /* all this means is that this executable hasn't been installed
         * yet for this app - let things continue so we record the
         * config for later
         */
        return true;
    }
    *exe = exec;
    /* is this version installed for this executable? */
    for (j=0; j < exec->versions.size; j++) {
        if (NULL == (vptr = (orcm_cfgi_version_t*)opal_pointer_array_get_item(&exec->versions, j))) {
            continue;
        }
        if (0 == strcmp(ctmp, vptr->version)) {
            OPAL_OUTPUT_VERBOSE((3, orcm_cfgi_base.output,
                                 "FOUND EXISTING VERSION %s IN EXEC %s",
                                 ctmp, exec->appname));
            *vers = vptr;
            return true;
        }
    }

    /* not found, but that just means it hasn't been installed yet */
    return true;
}

static bool find_binary(confd_hkeypath_t *kp,
                        orcm_cfgi_version_t *vers,
                        orcm_cfgi_run_t **runptr,
                        orcm_cfgi_bin_t **bin)
{
    confd_value_t *vp;
    char *instance, *app, *appname, *version;
    orcm_cfgi_run_t *run, *rptr;
    orcm_cfgi_app_t *aptr;
    orcm_cfgi_exec_t *exec;
    orcm_cfgi_bin_t *bptr;
    int i;

    /* set default */
    *runptr = NULL;
    *bin = NULL;

    /* get the name of the application */
    vp = qc_find_key(kp, orcm_app, 0);
    if (NULL == vp) {
        /* no key found */
        return false;
    }
    app = CONFD_GET_CBUFPTR(vp);
    /* find the instance this belongs to */
    vp = qc_find_key(kp, orcm_app_instance, 0);
    if (NULL == vp) {
        return false;
    }
    instance = CONFD_GET_CBUFPTR(vp);
    /* find the executable */
    vp = qc_find_key(kp, orcm_exec, 0);
    if (NULL == vp) {
        /* no exec key found */
        return false;
    }
    appname = CONFD_GET_CBUFPTR(vp);
    /* find the version */
    vp = qc_find_key(kp, orcm_version, 0);
    if (NULL == vp) {
        return false;
    }
    version = CONFD_GET_CBUFPTR(vp);
    run = NULL;
    for (i=0; i < orcm_cfgi_base.confgd_apps.size; i++) {
        if (NULL == (rptr = (orcm_cfgi_run_t*)opal_pointer_array_get_item(&orcm_cfgi_base.confgd_apps, i))) {
            continue;
        }
        if (NULL == rptr->application ||
            NULL == rptr->instance) {
            opal_output(0, "%s IMPROPERLY DEFINED CONFIG APPLICATION", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            return false;
        }
        if (0 == strcmp(rptr->application, app) &&
            0 == strcmp(rptr->instance, instance)) {
            run = rptr;
            break;
        }
    }
    if (NULL == run) {
        /* shouldn't happen - but better add it */
        run = OBJ_NEW(orcm_cfgi_run_t);
        run->application = strdup(app);
        run->instance = strdup(instance);
        run->idx = opal_pointer_array_add(&orcm_cfgi_base.confgd_apps, run);
    }
    /* return the run object pointer */
    *runptr = run;
    /* see if this version already exists in this instance */
    for (i=0; i < run->binaries.size; i++) {
        if (NULL == (bptr = (orcm_cfgi_bin_t*)opal_pointer_array_get_item(&run->binaries, i))) {
            continue;
        }
        if (0 == strcmp(bptr->appname, appname) &&
            0 == strcmp(bptr->version, version)) {
            *bin = bptr;
            if (NULL == bptr->vers && NULL != vers) {
                /* add it to the version for tracking purposes */
                bptr->vers_idx = opal_pointer_array_add(&vers->binaries, bptr);
                /* track the higher levels so we can cleanup as reqd */
                bptr->exec = vers->exec;
                bptr->vers = vers;
            }
            OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                 "%s FOUND EXISTING BINARY %s:%s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 appname, version));
            return true;
        }
    }
    /* if we get here, then this is a new version for this instance - so add it */
    bptr = OBJ_NEW(orcm_cfgi_bin_t);
    bptr->appname = strdup(appname);
    bptr->version = strdup(version);
    asprintf(&bptr->binary, "%s_%s", appname, version);
    /* add this binary to the running config */
    bptr->idx = opal_pointer_array_add(&run->binaries, bptr);
    if (NULL != vers) {
        /* also add it to the version for tracking purposes */
        bptr->vers_idx = opal_pointer_array_add(&vers->binaries, bptr);
        /* track the higher levels so we can cleanup as reqd */
        bptr->exec = vers->exec;
        bptr->vers = vers;
    }
    /* report out, if requested */
    if (0 < opal_output_get_verbosity(orcm_cfgi_base.output)) {
        opal_output(0, "%s CREATED NEW BINARY", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        orcm_cfgi_base_dump(NULL, NULL, bptr, ORCM_CFGI_BIN);
    }
    /* return the struct pointer */
    *bin = bptr;
    return true;

}

static boolean parse(confd_hkeypath_t *kp,
                     enum cdb_iter_op  op,
                     confd_value_t    *value,
	             enum cdb_sub_notification notify_type,
                     long which)
{
    confd_value_t *clist, *vp;
    char *cptr, *param, *ctmp;
    unsigned int i, imax;
    int32_t i32;
    int rc, j, app_idx;
    orte_job_t *jdat, *jdata=NULL;
    bool valid;
    boolean ret=FALSE;
    orcm_cfgi_caddy_t *caddy;
    opal_pointer_array_t *array;
    orcm_cfgi_app_t *installed_app;
    orcm_cfgi_app_t *app, *aptr;
    orcm_cfgi_exec_t *exec;
    orcm_cfgi_run_t *run, *rptr;
    orcm_cfgi_version_t *vers;
    orcm_cfgi_bin_t *bin;
    orte_app_context_t *ax, *axptr;
    char *application, *appname, *version;

    /* take control */
    ORTE_ACQUIRE_THREAD(&orcm_cfgi_base.ctl);

    if (NULL == kp) {
        /* process the cmd */
        if (CDB_SUB_COMMIT == notify_type) {
            for (j=0; j < active_apps.size; j++) {
                if (NULL == (caddy = (orcm_cfgi_caddy_t*)opal_pointer_array_get_item(&active_apps, j))) {
                    continue;
                }
                /* convenience */
                run = caddy->run;
                /* see if an installed application has been given - if not,
                 * then we just ignore this entry as all we really wanted to
                 * do was capture the config
                 */
                if (NULL == run->app) {
                    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                         "%s NOT SPAWNING CONFIGURED APP %s - APP NOT INSTALLED",
                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), run->application));
                    /* remove the entry from the array */
                    opal_pointer_array_set_item(&active_apps, j, NULL);
                    OBJ_RELEASE(caddy);
                    continue;
                }
                /* if this is a new job, we need to create a job object for it.
                 * if it is an existing job, we need to create a copy of the
                 * currently-executing job object and pass it to the base
                 * spawn routine, which compares the passed data against the
                 * currently-executing job's data to determine required changes.
                 * The base function will then update the run object's job data.
                 */
                jdata = OBJ_NEW(orte_job_t);
                caddy->jdata = jdata;
                jdata->name = strdup(run->application);
                jdata->instance = strdup(run->instance);
                for (i=0; i < run->binaries.size; i++) {
                    if (NULL == (bin = (orcm_cfgi_bin_t*)opal_pointer_array_get_item(&run->binaries, i))) {
                        continue;
                    }
                    if (NULL == bin->vers) {
                        /* corresponding binary has not been installed yet - ignore */
                        continue;
                    }
                    /* create an app_context for this binary */
                    ax = OBJ_NEW(orte_app_context_t);
                    ax->app = strdup(bin->binary);
                    /* copy the argv across */
                    if (NULL != bin->vers->argv) {
                        ax->argv = opal_argv_copy(bin->vers->argv);
                    }
                    /* stick the command at the beginning of the argv */
                    param = opal_basename(ax->app);
                    opal_argv_prepend_nosize(&ax->argv, param);
                    free(param);
                    /* set num procs */
                    ax->num_procs = bin->num_procs;
                    /* add it to the job */
                    ax->idx = opal_pointer_array_add(jdata->apps, ax);
                    jdata->num_apps++;
                }
                /* do a basic validity check on the job */
                if (ORCM_SUCCESS == orcm_cfgi_base_check_job(jdata)) {
                    /*spawn this job */
                    if (mca_orcm_cfgi_confd_component.test_mode) {
                        /* display the result */
                        orcm_cfgi_base_dump(NULL, NULL, caddy, ORCM_CFGI_CADDY);
                    } else {
                        if (0 < opal_output_get_verbosity(orcm_cfgi_base.output)) {
                            opal_output(0, "SPAWNING %s", jdata->name);
                            orcm_cfgi_base_dump(NULL, NULL, caddy, ORCM_CFGI_CADDY);
                        }
                        /* send it off to be processed */
                        if (ORCM_SUCCESS != (rc = opal_fd_write(orcm_cfgi_base.launch_pipe[1],
                                                                sizeof(orcm_cfgi_caddy_t*), &caddy))) {
                            ORTE_ERROR_LOG(rc);
                        }
                    }
                } else {
                    opal_output(0, "SPECIFIED APP %s IS INVALID - IGNORING",
                                (NULL == jdata->name) ? "NULL" : jdata->name);
                    opal_dss.dump(0, jdata, ORTE_JOB);
                    OBJ_RELEASE(jdata);
                    caddy->jdata = NULL;
                    OBJ_RELEASE(caddy);
                }
                /* clear this from the array */
                opal_pointer_array_set_item(&active_apps, j, NULL);
            }
        }
        ret = TRUE;
        goto release;
    }
    
    switch (op) {
    case MOP_CREATED:
        /* ignore created operations */
        ret = TRUE;
        goto release;
        break;
    case MOP_MODIFIED:
        /* ignore modify operations */
        ret = TRUE;
        goto release;
        break;
    case MOP_DELETED:
        vp = qc_find_key(kp, orcm_app, 0);
        if (NULL == vp) {
            /* no app was given, so kill all jobs */
            for (j=0; j < orcm_cfgi_base.confgd_apps.size; j++) {
                if (NULL == (run = (orcm_cfgi_run_t*)opal_pointer_array_get_item(&orcm_cfgi_base.confgd_apps, j))) {
                    continue;
                }
                /* adjust the number of instances */
                if (NULL != run->app) {
                    run->app->num_instances--;
                }
                /* return all the executable counts */
                for (i=0; i < run->binaries.size; i++) {
                    if (NULL == (bin = (orcm_cfgi_bin_t*)opal_pointer_array_get_item(&run->binaries, i))) {
                        continue;
                    }
                    if (NULL != bin->exec) {
                        bin->exec->total_procs -= bin->num_procs;
                    }
                }
                caddy = OBJ_NEW(orcm_cfgi_caddy_t);
                caddy->cmd = ORCM_CFGI_KILL_JOB;
                /* leave the run and jdata fields NULL as this indicates
                 * all jobs are to be terminated
                 */
                /* remove the run object from the array and release it */
                opal_pointer_array_set_item(&orcm_cfgi_base.confgd_apps, j, NULL);
                OBJ_RELEASE(run);
                /* send it off to be processed */
                opal_fd_write(orcm_cfgi_base.launch_pipe[1], sizeof(orcm_cfgi_caddy_t*), &caddy);
            }
            ret = TRUE;
            break;
        } else {
            cptr = CONFD_GET_CBUFPTR(vp);
            /* an app was specified - if no app-instance was given, then
             * terminate all running instances of that app
             */
            ctmp = NULL;
            vp = qc_find_key(kp, orcm_app_instance, 0);
            if (NULL != vp) {
                ctmp = CONFD_GET_CBUFPTR(vp);
            }
            /* see if an executable was given */
            appname = NULL;
            vp = qc_find_key(kp, orcm_exec, 0);
            if (NULL != vp) {
                appname = CONFD_GET_CBUFPTR(vp);
            }
            /* see if a version was given */
            version = NULL;
            vp = qc_find_key(kp, orcm_version, 0);
            if (NULL != vp) {
                version = CONFD_GET_CBUFPTR(vp);
            }
            /* search the configured app array */
            for (j=0; j < orcm_cfgi_base.confgd_apps.size; j++) {
                if (NULL == (run = (orcm_cfgi_run_t*)opal_pointer_array_get_item(&orcm_cfgi_base.confgd_apps, j))) {
                    continue;
                }
                if (0 == strcmp(cptr, run->application)) {
                    if (NULL == ctmp || 0 == strcmp(ctmp, run->instance)) {
                        if (NULL == appname) {
                            /**** applies to the entire running instance ****/
                            caddy = OBJ_NEW(orcm_cfgi_caddy_t);
                            caddy->cmd = ORCM_CFGI_KILL_JOB;
                            /* remove the config tracker to match what is in confd */
                            opal_pointer_array_set_item(&orcm_cfgi_base.confgd_apps, j, NULL);
                            /* adjust the number of instances */
                            if (NULL != run->app) {
                                run->app->num_instances--;
                            }
                            /* return all the executable counts */
                            for (i=0; i < run->binaries.size; i++) {
                                if (NULL == (bin = (orcm_cfgi_bin_t*)opal_pointer_array_get_item(&run->binaries, i))) {
                                    continue;
                                }
                                if (NULL != bin->exec) {
                                    bin->exec->total_procs -= bin->num_procs;
                                }
                            }
                            /* don't retain the run object as it has been removed from confd,
                             * let the caddy destructor destroy it
                             */
                            caddy->run = run;
                            /* send it off to be processed */
                            opal_fd_write(orcm_cfgi_base.launch_pipe[1], sizeof(orcm_cfgi_caddy_t*), &caddy);
                        } else if (NULL == version) {
                            /**** terminate all versions of the specified executable ****/
                            /* this is a little trickier as we aren't killing an entire job,
                             * but instead just specific processes in that job. This is done
                             * by setting the num_procs for that executable to zero
                             */
                            OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                                 "%s ORDERING APP %s INSTANCE %s EXEC %s (all versions) TO ABORT",
                                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                                 run->application, run->instance, appname));
                            valid = false;
                            /* adjust the running config */
                            for (i=0; i < run->binaries.size; i++) {
                                if (NULL == (bin = (orcm_cfgi_bin_t*)opal_pointer_array_get_item(&run->binaries, i))) {
                                    continue;
                                }
                                if (0 == strcmp(appname, bin->appname)) {
                                    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                                         "%s ORDERING EXECUTABLE %s VERSION %s TO ABORT",
                                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), bin->appname, bin->version));
                                    if (NULL != bin->exec) {
                                        bin->exec->total_procs -= bin->num_procs;
                                    }
                                    bin->vers = NULL;
                                    valid = true;
                                    /* might be multiple matches since we are covering
                                     * multiple versions, so keep going
                                     */
                                }
                            }
                            if (valid) {
                                caddy = OBJ_NEW(orcm_cfgi_caddy_t);
                                caddy->cmd = ORCM_CFGI_KILL_EXE;
                                /* retain the run object as it has -not- been removed from confd */
                                OBJ_RETAIN(run);
                                caddy->run = run;
                                /* tell the base function to remove all zero'd binaries */
                                caddy->cleanup = true;
                                /* send it off to be processed */
                                opal_fd_write(orcm_cfgi_base.launch_pipe[1], sizeof(orcm_cfgi_caddy_t*), &caddy);
                            }
                        } else {
                            /**** terminate -only- the specified version of this executable ****/
                            /* tricky again - see above comment */
                            OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                                 "%s ORDERING APP %s INSTANCE %s EXEC %s VERSION %s TO ABORT",
                                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                                 run->application, run->instance, appname, version));
                            valid = false;
                            /* adjust the running config */
                            for (i=0; i < run->binaries.size; i++) {
                                if (NULL == (bin = (orcm_cfgi_bin_t*)opal_pointer_array_get_item(&run->binaries, i))) {
                                    continue;
                                }
                                if (0 == strcmp(appname, bin->appname) &&
                                    0 == strcmp(version, bin->version)) {
                                    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                                         "%s SETTING EXECUTABLE %s VERSION %s TO ABORT",
                                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), appname, version));
                                    if (NULL != bin->exec) {
                                        bin->exec->total_procs -= bin->num_procs;
                                    }
                                    bin->vers = NULL;
                                    valid = true;
                                    break;
                                }
                            }
                            if (valid) {
                                caddy = OBJ_NEW(orcm_cfgi_caddy_t);
                                caddy->cmd = ORCM_CFGI_KILL_EXE;
                                /* retain the run object as it has -not- been removed from confd */
                                OBJ_RETAIN(run);
                                caddy->run = run;
                                /* tell the base function to remove all zero'd binaries */
                                caddy->cleanup = true;
                                /* send it off to be processed */
                                opal_fd_write(orcm_cfgi_base.launch_pipe[1], sizeof(orcm_cfgi_caddy_t*), &caddy);
                            }
                        }
                    }
                }
            }
        }
        ret = TRUE;
        break;

    case MOP_VALUE_SET:
        opal_output_verbose(2, orcm_cfgi_base.output, "VALUE_SET");

        switch(qc_get_xmltag(kp,1)) {
            /* JOB-LEVEL VALUES */
        case orcm_app_instance:
            if (!find_app(kp, &app, &application)) {
                /* the app key wasn't provided */
                ret = FALSE;
                break;
            }
            cptr = CONFD_GET_CBUFPTR(value);
            /* instance name for the given app -must- be unique */
            run = NULL;
            for (i=0; i < orcm_cfgi_base.confgd_apps.size; i++) {
                if (NULL == (rptr = (orcm_cfgi_run_t*)opal_pointer_array_get_item(&orcm_cfgi_base.confgd_apps, i))) {
                    continue;
                }
                if (0 != strcmp(rptr->application, application)) {
                    continue;
                }
                if (0 == strcmp(rptr->instance, cptr)) {
                    run = rptr;
                    break;
                }
            }
            if (NULL == app) {
                /* the specified app has not yet been installed. In this case,
                 * we still want to capture the config so we can start the
                 * app-instance if/when the referenced app ever does get installed
                 */
                if (NULL == run) {
                    /* don't have this config yet - add it */
                    run = OBJ_NEW(orcm_cfgi_run_t);
                    run->application = application;
                    run->instance = strdup(cptr);
                    run->idx = opal_pointer_array_add(&orcm_cfgi_base.confgd_apps, run);
                }
                ret = TRUE;
                break;
            }
            /* specified app has been installed - configure it */
            if (app->max_instances < 0 || app->num_instances < app->max_instances) {
                if (NULL == run) {
                    /* don't have this config yet - add it */
                    run = OBJ_NEW(orcm_cfgi_run_t);
                    run->app = app;
                    run->application = application;
                    run->instance = strdup(cptr);
                    run->idx = opal_pointer_array_add(&orcm_cfgi_base.confgd_apps, run);
                }
                /* add it to our array of apps waiting for commit */
                caddy = OBJ_NEW(orcm_cfgi_caddy_t);
                caddy->cmd = ORCM_CFGI_SPAWN;
                /* retain the run object so the caddy release doesn't release it */
                OBJ_RETAIN(run);
                caddy->run = run;
                opal_pointer_array_add(&active_apps, caddy);
                if (3 < opal_output_get_verbosity(orcm_cfgi_base.output)) {
                    opal_output(0, "NEW CADDY");
                    orcm_cfgi_base_dump(NULL, NULL, caddy, ORCM_CFGI_CADDY);
                }
                /* track it against the installed apps */
                run->app_idx = opal_pointer_array_add(&app->instances, run);
                app->num_instances++;
            } else {
                opal_output(0, "%s APP %s: MAX NUMBER OF ALLOWED INSTANCES (%d) EXCEEDED - CANNOT CONFIGURE INSTANCE %s",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            (NULL == app->application) ? "NULL" : app->application,
                            app->max_instances, cptr);
                free(application);
                ret = FALSE;
                break;
            }
            ret = TRUE;
            break;

            /* APP-LEVEL VALUES */
        case orcm_app_name:
        case orcm_exec:
        case orcm_exec_name:
        case orcm_version_name:
            /* not a supported option. However, confd does a stupid thing
             * and feeds us these values as it "builds" to other options
             * that -are- supported, so just ignore them here
             */
            ret = TRUE;
            break;

        case orcm_count:
            i32 = CONFD_GET_INT32(value);
            if (!find_version(kp, &exec, &vers)) {
                /* the app/exec/version keys weren't provided */
                ret = FALSE;
                break;
            }
            if (!find_binary(kp, vers, &run, &bin)) {
                /* the version key wasn't provided */
                ret = FALSE;
                break;
            }
            if (NULL != exec) {
                /* see if this would give us too many procs - obviously, if
                 * the executable hasn't been installed yet, we can't know
                 * and will just have to assume it's okay for now
                 */
                if (0 <= exec->process_limit) {
                    if (exec->process_limit < ((i32 - bin->num_procs) + exec->total_procs)) {
                        opal_output(0, "%s EXECUTABLE %s: MAX NUMBER OF ALLOWED PROCS (%d) EXCEEDED - CANNOT ADD %d PROCS, ALREADY HAVE %d",
                                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                    (NULL == exec->appname) ? "NULL" : exec->appname,
                                    exec->process_limit, i32 - bin->num_procs, exec->total_procs);
                        ret = FALSE;
                        break;
                    }
                }
                exec->total_procs += (i32 - bin->num_procs);
            }
            bin->num_procs = i32;
            /* if this run object isn't already in the active_apps array,
             * add it - this happens when you change the num_procs for an
             * executable, or add an executable to a running job
             */
            valid = false;
            for (i=0; i < active_apps.size; i++) {
                if (NULL == (caddy = (orcm_cfgi_caddy_t*)opal_pointer_array_get_item(&active_apps, i))) {
                    continue;
                }
                if (run == caddy->run) {
                    valid = true;
                    break;
                }
            }
            if (!valid) {
                /* need to add it */
                caddy = OBJ_NEW(orcm_cfgi_caddy_t);
                caddy->cmd = ORCM_CFGI_SPAWN;
                /* retain the run object so the caddy release doesn't release it */
                OBJ_RETAIN(run);
                caddy->run = run;
                opal_pointer_array_add(&active_apps, caddy);
            }
            ret = TRUE;
            break;

            /* IGNORED VALUES */
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
    /* release control */
    ORTE_RELEASE_THREAD(&orcm_cfgi_base.ctl);
    return ret;
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
    orte_job_t *jdat, *jdata;
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
    orte_app_context_t *app;
    struct confd_decimal64 d64;

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
        if (NULL != vp) {
            jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        }
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
        if (NULL != vp) {
            jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
        }
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
        cp = opal_basename(app->app);
        CONFD_SET_CBUF(&val, cp, strlen(cp));
        confd_data_reply_value(tctx, &val);
        free(cp);
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
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, CONFD_GET_UINT32(vp));
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
        cp = opal_basename(app->app);
        CONFD_SET_CBUF(&val, cp, strlen(cp));
        confd_data_reply_value(tctx, &val);
        free(cp);
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

    case orcm_num_threads:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NUM THREADS ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR NUM THREADS - NO JOB PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.jobid = ORTE_CONSTRUCT_LOCAL_JOBID(ORTE_PROC_MY_NAME->jobid, CONFD_GET_UINT32(vp));;
        vp = qc_find_key(kp, orcm_replica, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR NUM THREADS - NO REPLICA PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.vpid = CONFD_GET_UINT32(vp);
        /* find this process */
        proc = get_child(&name);
        if (NULL == proc) {
            opal_output(0, "%s CONFD REQUEST FOR NUM THREADS - PROC %s NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&name));
            goto notfound;
        }
        CONFD_SET_INT16(&val, proc->stats.num_threads);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_percent_cpu:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR PERCENT CPU ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR PERCENT CPU - NO JOB PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.jobid = ORTE_CONSTRUCT_LOCAL_JOBID(ORTE_PROC_MY_NAME->jobid, CONFD_GET_UINT32(vp));;
        vp = qc_find_key(kp, orcm_replica, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR PERCENT CPU - NO REPLICA PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.vpid = CONFD_GET_UINT32(vp);
        /* find this process */
        proc = get_child(&name);
        if (NULL == proc) {
            opal_output(0, "%s CONFD REQUEST FOR PERCENT CPU - PROC %s NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&name));
            goto notfound;
        }
        d64.value = (int)(proc->stats.percent_cpu * 100.0);
        d64.fraction_digits = 2;
        CONFD_SET_DECIMAL64(&val, d64);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_vsize:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR VSIZE ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR VSIZE - NO JOB PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.jobid = ORTE_CONSTRUCT_LOCAL_JOBID(ORTE_PROC_MY_NAME->jobid, CONFD_GET_UINT32(vp));;
        vp = qc_find_key(kp, orcm_replica, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR VSIZE - NO REPLICA PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.vpid = CONFD_GET_UINT32(vp);
        /* find this process */
        proc = get_child(&name);
        if (NULL == proc) {
            opal_output(0, "%s CONFD REQUEST FOR VSIZE - PROC %s NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&name));
            goto notfound;
        }
        d64.value = (int)(proc->stats.vsize * 100.0);
        d64.fraction_digits = 2;
        CONFD_SET_DECIMAL64(&val, d64);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_rss:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR RSS ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR RSS - NO JOB PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.jobid = ORTE_CONSTRUCT_LOCAL_JOBID(ORTE_PROC_MY_NAME->jobid, CONFD_GET_UINT32(vp));;
        vp = qc_find_key(kp, orcm_replica, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR RSS - NO REPLICA PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.vpid = CONFD_GET_UINT32(vp);
        /* find this process */
        proc = get_child(&name);
        if (NULL == proc) {
            opal_output(0, "%s CONFD REQUEST FOR RSS - PROC %s NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&name));
            goto notfound;
        }
        d64.value = (int)(proc->stats.rss * 100.0);
        d64.fraction_digits = 2;
        CONFD_SET_DECIMAL64(&val, d64);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_peak_vsize:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR PEAK VSIZE ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR PEAK VSIZE - NO JOB PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.jobid = ORTE_CONSTRUCT_LOCAL_JOBID(ORTE_PROC_MY_NAME->jobid, CONFD_GET_UINT32(vp));;
        vp = qc_find_key(kp, orcm_replica, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR PEAK VSIZE - NO REPLICA PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.vpid = CONFD_GET_UINT32(vp);
        /* find this process */
        proc = get_child(&name);
        if (NULL == proc) {
            opal_output(0, "%s CONFD REQUEST FOR PEAK VSIZE - PROC %s NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&name));
            goto notfound;
        }
        d64.value = (int)(proc->stats.peak_vsize * 100.0);
        d64.fraction_digits = 2;
        CONFD_SET_DECIMAL64(&val, d64);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_processor:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR PROCESSOR ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR PROCESSOR - NO JOB PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.jobid = ORTE_CONSTRUCT_LOCAL_JOBID(ORTE_PROC_MY_NAME->jobid, CONFD_GET_UINT32(vp));;
        vp = qc_find_key(kp, orcm_replica, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR PROCESSOR - NO REPLICA PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.vpid = CONFD_GET_UINT32(vp);
        /* find this process */
        proc = get_child(&name);
        if (NULL == proc) {
            opal_output(0, "%s CONFD REQUEST FOR PROCESSOR - PROC %s NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&name));
            goto notfound;
        }
        CONFD_SET_INT16(&val, proc->stats.processor);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_process_state:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR PROCESS STATE ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_job, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR PROCESS STATE - NO JOB PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.jobid = ORTE_CONSTRUCT_LOCAL_JOBID(ORTE_PROC_MY_NAME->jobid, CONFD_GET_UINT32(vp));;
        vp = qc_find_key(kp, orcm_replica, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR PROCESS STATE - NO REPLICA PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        name.vpid = CONFD_GET_UINT32(vp);
        /* find this process */
        proc = get_child(&name);
        if (NULL == proc) {
            opal_output(0, "%s CONFD REQUEST FOR PROCESS STATE - PROC %s NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&name));
            goto notfound;
        }
        CONFD_SET_CBUF(&val, proc->stats.state, strlen(proc->stats.state));
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

        /****  NODE DATA  ****/
    case orcm_node_id:
        CONFD_SET_CBUF(&val, "NODEID", strlen("NODEID"));
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_node_name:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE NAME ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR NODE NAME - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_UINT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR NODE NAME - NODE %u NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_UINT32(vp));
            goto notfound;
        }
        if (NULL == node->name) {
            CONFD_SET_CBUF(&val, "UNKNOWN", strlen("UNKNOWN"));
        } else {
            CONFD_SET_CBUF(&val, node->name, strlen(node->name));
        }
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_state:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE STATE ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR NODE STATE - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_UINT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR NODE STATE - NODE %u NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_UINT32(vp));
            goto notfound;
        }
        switch (node->state) {
        case ORTE_NODE_STATE_UP:
        case ORTE_NODE_STATE_NOT_INCLUDED:
        case ORTE_NODE_STATE_DO_NOT_USE:
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

#if 0
    case orcm_temperature:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE TEMP ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR NODE TEMP - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_UINT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR NODE TEMP - NODE %u NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_UINT32(vp));
            goto notfound;
        }
        CONFD_SET_INT8(&val, 1);
        confd_data_reply_value(tctx, &val);
        goto release;
#endif

    case orcm_num_procs:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE NUM PROCS ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR NODE NUM PROCS - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_UINT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR NODE NUM PROCS - NODE %u NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_UINT32(vp));
            goto notfound;
        }
        i32 = node->num_procs;
        CONFD_SET_INT32(&val, i32);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_total_memory:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE TOTAL MEM ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR TOTAL MEM - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_UINT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR TOTAL MEM - NODE %u NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_UINT32(vp));
            goto notfound;
        }
        d64.value = (int)(node->stats.total_mem * 100.0);
        d64.fraction_digits = 2;
        CONFD_SET_DECIMAL64(&val, d64);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_free_memory:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE FREE MEM ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR FREE MEM - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_UINT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR FREE MEM - NODE %u NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_UINT32(vp));
            goto notfound;
        }
        d64.value = (int)(node->stats.free_mem * 100.0);
        d64.fraction_digits = 2;
        CONFD_SET_DECIMAL64(&val, d64);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_load_avg:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE LOAD AVG ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR LOAD AVG - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_UINT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR LOAD AVG - NODE %u NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_UINT32(vp));
            goto notfound;
        }
        d64.value = (int)(node->stats.la * 100.0);
        d64.fraction_digits = 2;
        CONFD_SET_DECIMAL64(&val, d64);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_load_avg5:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE LOAD AVG ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR LOAD AVG5 - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_UINT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR LOAD AVG5 - NODE %u NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_UINT32(vp));
            goto notfound;
        }
        d64.value = (int)(node->stats.la5 * 100.0);
        d64.fraction_digits = 2;
        CONFD_SET_DECIMAL64(&val, d64);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_load_avg15:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE LOAD AVG15 ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR LOAD AVG15 - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_UINT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR LOAD AVG15 - NODE %u NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_UINT32(vp));
            goto notfound;
        }
        d64.value = (int)(node->stats.la15 * 100.0);
        d64.fraction_digits = 2;
        CONFD_SET_DECIMAL64(&val, d64);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_buffers:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE BUFFERS ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR BUFFERS - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_UINT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR BUFFERS - NODE %u NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_UINT32(vp));
            goto notfound;
        }
        d64.value = (int)(node->stats.buffers * 100.0);
        d64.fraction_digits = 2;
        CONFD_SET_DECIMAL64(&val, d64);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_cached:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE CACHED ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR CACHED - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_UINT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR CACHED - NODE %u NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_UINT32(vp));
            goto notfound;
        }
        d64.value = (int)(node->stats.cached * 100.0);
        d64.fraction_digits = 2;
        CONFD_SET_DECIMAL64(&val, d64);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_swap_cached:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE SWAPCACHED ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR SWAPCACHED - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_UINT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR SWAPCACHED - NODE %u NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_UINT32(vp));
            goto notfound;
        }
        d64.value = (int)(node->stats.swap_cached * 100.0);
        d64.fraction_digits = 2;
        CONFD_SET_DECIMAL64(&val, d64);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_swap_total:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE SWAPTOTAL ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR SWAPTOTAL - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_UINT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR SWAPTOTAL - NODE %u NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_UINT32(vp));
            goto notfound;
        }
        d64.value = (int)(node->stats.swap_total * 100.0);
        d64.fraction_digits = 2;
        CONFD_SET_DECIMAL64(&val, d64);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_swap_free:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE SWAPFREE ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR SWAPFREE - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_UINT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR SWAPFREEA - NODE %u NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_UINT32(vp));
            goto notfound;
        }
        d64.value = (int)(node->stats.swap_free * 100.0);
        d64.fraction_digits = 2;
        CONFD_SET_DECIMAL64(&val, d64);
        confd_data_reply_value(tctx, &val);
        goto release;

    case orcm_mapped:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NODE MAPPED ELEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        vp = qc_find_key(kp, orcm_node, 0);
        if (NULL == vp) {
            opal_output(0, "%s CONFD REQUEST FOR MAPPED - NO NODE INDEX PROVIDED",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            goto notfound;
        }
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, CONFD_GET_UINT32(vp)))) {
            opal_output(0, "%s CONFD REQUEST FOR MAPPED - NODE %u NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), CONFD_GET_UINT32(vp));
            goto notfound;
        }
        d64.value = (int)(node->stats.mapped * 100.0);
        d64.fraction_digits = 2;
        CONFD_SET_DECIMAL64(&val, d64);
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
    opal_pointer_array_t *array;

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
            goto notfound;
        }
        ui32 = CONFD_GET_UINT32(ky);
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, ui32);
        if (NULL == jdat) {
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
        ui32 = CONFD_GET_UINT32(ky);
        jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, ui32);
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

    case orcm_node:
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s REQUEST FOR NEXT NODE",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        /* look for next non-NULL node */
        if (next == -1) {
            /* ignore the node for orcm-sched and orcm master
             * as the actual local daemon will pick it up for us
             * or else we'll see that node displayed twice
             */
            next = 2;
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


static boolean orcm_clear(int maapisock,
                          struct confd_user_info *uinfo,
                          int argc, char **argv, long which)
{
    int i, n;
    char *value, *appname, *remainder;
    orte_job_t *jdata=NULL, *jdt;
    orte_app_context_t *app=NULL, *aptr;
    orte_proc_t *proc=NULL;
    boolean ret = TRUE;
    bool restart;
    orcm_cfgi_caddy_t *caddy;

    if (1 < opal_output_get_verbosity(orcm_cfgi_base.output)) {
        opal_output(0, "%s CMDPOINT CALLED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        for (i=0; i < argc; i++) {
            opal_output(0, "%s     %s", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), argv[i]);
        }
    }

    /* cycle through the argv looking for reqd command elements */
    for (i=0; i < argc; i++) {
        /* get ptr to the value */
        value = strchr(argv[i], '=');
        if (NULL == value) {
            /* no equals sign - that's wrong */
            opal_output(0, "%s VALUE MISSING EQUAL SIGN: %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), argv[i]);
            ret = FALSE;
            goto cleanup;
        }
        value++;
        if (NULL == value) {
            /* no value present - that's wrong */
            opal_output(0, "%s VALUE MISSING: %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), argv[i]);
            ret = FALSE;
            goto cleanup;
        }

        if (0 == strncmp(argv[i], "JN", strlen("JN"))) {
            /* see if we already found it - that would be wrong */
            if (NULL != jdata) {
                opal_output(0, "%s JOB %s DOUBLY-SPECIFIED",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), jdata->name);
                ret = FALSE;
                goto cleanup;
            }
            /* find the job of this name */
            jdata = NULL;
            for (n=1; n < orte_job_data->size; n++) {
                if (NULL == (jdt = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, n))) {
                    continue;
                }
                if (0 == strcmp(jdt->name, value)) {
                    jdata = jdt;
                    break;
                }
            }
            if (NULL == jdata) {
                opal_output(0, "%s JOB %s NOT FOUND",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), value);
                ret = FALSE;
                goto cleanup;
            }
        } else if (0 == strncmp(argv[i], "JI", strlen("JI"))) {
            /* see if we already found it - that would be wrong */
            if (NULL != jdata) {
                opal_output(0, "%s JOB %s DOUBLY-SPECIFIED",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), jdata->name);
                ret = FALSE;
                goto cleanup;
            }
            /* find this job number - need extra protection here as
             * confd does -not- check types on input data! Thus, a user
             * could have provided a string, or have a typo character
             * in the middle of the number
             */
            n = strtol(value, &remainder, 10);
            if (NULL != remainder && 0 < strlen(remainder)) {
                /* there was indeed a character in here! */
                opal_output(0, "%s JOB ID %s CONTAINS NON-NUMERIC CHARACTER",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), value);
                ret = FALSE;
                goto cleanup;
            }
           /* get this job number */
            if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, n))) {
            }
        } else if (0 == strncmp(argv[i], "EXE", strlen("EXE"))) {
            /* must have defined the job first */
            if (NULL == jdata) {
                opal_output(0, "%s JOB FOR APP %s NOT SPECIFIED",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), value);
                ret = FALSE;
                goto cleanup;
            }
            /* look for executable */
            app = NULL;
            for (n=0; n < jdata->apps->size; n++) {
                if (NULL == (aptr = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, n))) {
                    continue;
                }
                appname = opal_basename(aptr->app);
                if (0 == strcmp(appname, value)) {
                    app = aptr;
                    free(appname);
                    break;
                }
                free(appname);
            }
            if (NULL == app) {
                /* couldn't find this app */
                opal_output(0, "%s APP %s IN JOB %s NOT FOUND",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), value, jdata->name);
                ret = FALSE;
                goto cleanup;
            }
        } else if (0 == strncmp(argv[i], "VPID", strlen("VPID"))) {
            if (NULL == jdata) {
                opal_output(0, "%s JOB NOT YET DEFINED",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
                goto cleanup;
            }
            /* get this vpid - need extra protection here as
             * confd does -not- check types on input data! Thus, a user
             * could have provided a string, or have a typo character
             * in the middle of the number
             */
            n = strtol(value, &remainder, 10);
            if (NULL != remainder && 0 < strlen(remainder)) {
                /* there was indeed a character in here! */
                opal_output(0, "%s PROC ID %s CONTAINS NON-NUMERIC CHARACTER",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), value);
                ret = FALSE;
                goto cleanup;
            }
            if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, n))) {
                opal_output(0, "%s PROC %d IN JOB %s IS NOT AVAILABLE",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), n, jdata->name);
                ret = FALSE;
                goto cleanup;
            }
            /* bozo check - did they specify an executable, but give
             * us a proc that is not from that executable?
             */
            if (NULL != app && app->idx != proc->app_idx) {
                appname = opal_basename(app->app);
                opal_output(0, "%s PROC %s DOES NOT BELONG TO EXECUTABLE %s",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            ORTE_NAME_PRINT(&proc->name), appname);
                free(appname);
                ret = FALSE;
                goto cleanup;
            }
        }
    }

    /* check for bozo case - should not be possible as confd
     * won't let you do it, but...
     */
    if (NULL == jdata) {
        opal_output(0, "%s NO JOB SPECIFIED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        ret = FALSE;
        goto cleanup;
    }

    /* the number of parameters we received in argv depends upon
     * the level of input received from the user. For example, the
     * user may want to reset all the procs in a job, and so will
     * only provide the job as input. All combinations can be given.
     * We could receive an executable and no vpid, or we could receive
     * a vpid without an executable. So we need to consider a number
     * of cases
     */

    restart = false;
    if (NULL == app && NULL == proc) {
        /* only give a job - reset all procs in that job */
        for (n=0; n < jdata->procs->size; n++) {
            if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, n))) {
                continue;
            }
            OPAL_OUTPUT_VERBOSE((5, orcm_cfgi_base.output,
                                 "RESETTING PROC %s", ORTE_NAME_PRINT(&proc->name)));
            proc->restarts = 0;
            if (ORTE_PROC_STATE_CANNOT_RESTART == proc->state) {
                /* if the proc hit its max restarts and couldn't be
                 * restarted, we need to restart it now
                 */
                jdata->state = ORTE_JOB_STATE_RESTART;
                proc->state = ORTE_PROC_STATE_RESTART;
                restart = true;
            }
        }
    } else if (NULL != proc) {
        /* only one proc specified - reset it */
        OPAL_OUTPUT_VERBOSE((5, orcm_cfgi_base.output,
                             "RESETTING PROC %s", ORTE_NAME_PRINT(&proc->name)));
        proc->restarts = 0;
        if (ORTE_PROC_STATE_CANNOT_RESTART == proc->state) {
            /* if the proc hit its max restarts and couldn't be
             * restarted, we need to restart it now
             */
            jdata->state = ORTE_JOB_STATE_RESTART;
            proc->state = ORTE_PROC_STATE_RESTART;
            restart = true;
        }
    } else {
        /* only way here is for app != NULL and proc == NULL,
         * indicating that we are to reset all procs in that app
         */
        for (n=0; n < jdata->procs->size; n++) {
            if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, n))) {
                continue;
            }
            if (proc->app_idx == app->idx) {
                OPAL_OUTPUT_VERBOSE((5, orcm_cfgi_base.output,
                                     "RESETTING PROC %s", ORTE_NAME_PRINT(&proc->name)));
                proc->restarts = 0;
                if (ORTE_PROC_STATE_CANNOT_RESTART == proc->state) {
                    /* if the proc hit its max restarts and couldn't be
                     * restarted, we need to restart it now
                     */
                    jdata->state = ORTE_JOB_STATE_RESTART;
                    proc->state = ORTE_PROC_STATE_RESTART;
                    restart = true;
                }
            }
        }
    }

    if (restart) {
        /* issue the restart command */
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "RESTARTING JOB %s", jdata->name));
        caddy = OBJ_NEW(orcm_cfgi_caddy_t);
        caddy->cmd = ORCM_CFGI_SPAWN;
        caddy->jdata = jdata;
        opal_fd_write(orcm_cfgi_base.launch_pipe[1], sizeof(orcm_cfgi_caddy_t*), &caddy);
    }

 cleanup:
    return ret;
}

static boolean orcm_clear_completion(struct confd_user_info *uinfo,
                                     int                     cli_style,
                                     char                   *token,
                                     int                     comp_char,
                                     confd_hkeypath_t       *kp,
                                     char                   *cmdpath,
                                     char                   *param_id)
{
    struct confd_completion_value *cmplt=NULL, value;
    int i, n, nopts;
    orte_job_t *jdata, *jdt;
    orte_app_context_t *app, *aptr;
    orte_proc_t *proc;
    boolean ret=TRUE;
    char **options=NULL;
    char *appname, *numid, *remainder;

    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                         "%s CLEAR COMPLETION CALLED\n    token %s(%d)\n     cmdpath %s\n     paramid %s\n",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == token) ? "NULL" : token,
                         (NULL == token) ? 0 : (int)strlen(token),
                         (NULL == cmdpath) ? "NULL" : cmdpath,
                         (NULL == param_id) ? "NULL" : param_id));

    options = opal_argv_split(cmdpath, ' ');
    nopts = opal_argv_count(options);

    if (0 == strcmp(options[nopts-1], "job-name") ||
        0 == strcmp(options[nopts-1], "job-id")) {
        /* count the number of active jobs */
        n=0;
        for (i=1; i < orte_job_data->size; i++) {
            if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
                continue;
            }
            n++;
        }

        if (0 == n) {
            /* no running jobs */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "No active jobs";
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            return ret;
        }

        /* allocate the completion array - this is the largest the array of completions
         * can be. We may not fill it all, so we track the actual number of entries
         * and pass that value back to confd
         */
        cmplt = (struct confd_completion_value *)calloc(n, sizeof(struct confd_completion_value));

        if (0 == strcmp(options[nopts-1], "job-name")) {
            /* were we given a partial? */
            if (0 == strlen(token)) {
                /* nope - send back all the job names */
                for (i=1, n=0; i < orte_job_data->size; i++) {
                    if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
                        continue;
                    }
                    if (NULL == jdata->name) {
                        continue;
                    }
                    cmplt[n].type = CONFD_COMPLETION;
                    cmplt[n].value = strdup(jdata->name);
                    cmplt[n].extra = NULL;
                    n++;
                }
            } else {
                /* yes - just insert those that start with token */
                for (i=1, n=0; i < orte_job_data->size; i++) {
                    if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
                        continue;
                    }
                    if (NULL == jdata->name) {
                        continue;
                    }
                    if (0 == strncmp(jdata->name, token, strlen(token))) {
                        cmplt[n].type = CONFD_COMPLETION;
                        cmplt[n].value = strdup(jdata->name);
                        cmplt[n].extra = NULL;
                        n++;
                    }
                }
            }
        } else if (0 == strcmp(options[nopts-1], "job-id")) {
            /* were we given a partial? */
            if (0 == strlen(token)) {
                /* nope - send back all the job ids */
                for (i=1, n=0; i < orte_job_data->size; i++) {
                    if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
                        continue;
                    }
                    cmplt[n].type = CONFD_COMPLETION;
                    asprintf(&cmplt[n].value, "%d", i);
                    cmplt[n].extra = NULL;
                    n++;
                }
            } else {
                /* check to see if we were given a number. Confd does
                 * not check for type match before sending us data, so
                 * the user could be typing gibberish
                 */
                n = strtol(token, &remainder, 10);
                if (NULL != remainder && 0 < strlen(remainder)) {
                    opal_output(0, "%s COMPLETION ERROR: JOB-ID %s HAS NON-NUMERIC CHARACTER %s %d",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), token, remainder, (int)strlen(remainder));
                    ret = FALSE;
                    goto cleanup;
                }
                /* just insert those that start with token. This is
                 * a little ugly as the token is a string and the job number
                 * is an integer - hopefully, someone will devise a more
                 * efficient method!
                 */
                for (i=1, n=0; i < orte_job_data->size; i++) {
                    if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
                        continue;
                    }
                    asprintf(&numid, "%d", i);
                    if (0 == strncmp(numid, token, strlen(token))) {
                        cmplt[n].type = CONFD_COMPLETION;
                        cmplt[n].value = strdup(numid);
                        cmplt[n].extra = NULL;
                        n++;
                    }
                    free(numid);
                }
            }
        }

        if (confd_action_reply_completion(uinfo, cmplt, n) < 0) {
            opal_output(0, "%s COMPLETION FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            ret = FALSE;
        }
    } else if (0 == strcmp(options[nopts-1], "executable")) {
        /* the job name or job-id is in the third position */
        if (0 == strcmp(options[2], "job-name")) {
            /* find this job name */
            jdata = NULL;
            for (i=1; i < orte_job_data->size; i++) {
                if (NULL == (jdt = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
                    continue;
                }
                if (NULL == jdt->name) {
                    continue;
                }
                if (0 == strcmp(jdt->name, options[3])) {
                    jdata = jdt;
                    break;
                }
            }
            if (NULL == jdata) {
                opal_output(0, "%s COMPLETION ERROR: JOB %s NOT FOUND",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            options[3]);
                ret = FALSE;
                goto cleanup;
            }
        } else if (0 == strcmp(options[2], "job-id")) {
            /* find this job number - need extra protection here as
             * confd does -not- check types on input data! Thus, a user
             * could have provided a string, or have a typo character
             * in the middle of the number
             */
            i = strtol(options[3], &remainder, 10);
            if (NULL != remainder && 0 < strlen(remainder)) {
                /* there was indeed a character in here! */
                opal_output(0, "%s COMPLETION ERROR: JOB-ID %s HAS NON-NUMERIC CHARACTER %s(%d)",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), options[3], remainder, (int)strlen(remainder));
                ret = FALSE;
                goto cleanup;
            }
            if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
                opal_output(0, "%s COMPLETION ERROR: JOB %s NOT FOUND",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            options[3]);
                ret = FALSE;
                goto cleanup;
            }
        } else {
            /* unrecognized */
            opal_output(0, "%s COMPLETION ERROR: UNRECOGNIZED PATH ELEMENT %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        options[2]);
            ret = FALSE;
            goto cleanup;
        }
        /* allocate the completion array - this is the largest the array of completions
         * can be. We may not fill it all, so we track the actual number of entries
         * and pass that value back to confd
         */
        cmplt = (struct confd_completion_value *)calloc(jdata->num_apps, sizeof(struct confd_completion_value));

        /* were we given a partial token? */
        if (0 == strlen(token)) {
            /* no - get all completions for this job */
            for (i=0; i < jdata->apps->size; i++) {
                if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, i))) {
                    continue;
                }
                appname = opal_basename(app->app);
                cmplt[i].type = CONFD_COMPLETION;
                cmplt[i].value = strdup(appname);
                cmplt[i].extra = NULL;
                free(appname);
            }
            n = i;
        } else {
            /* yes - get all completions that start with provided token */
            for (i=0, n=0; i < jdata->apps->size; i++) {
                if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, i))) {
                    continue;
                }
                appname = opal_basename(app->app);
                if (0 == strncmp(appname, token, strlen(token))) {
                    cmplt[n].type = CONFD_COMPLETION;
                    cmplt[n].value = strdup(appname);
                    cmplt[n].extra = NULL;
                    n++;
                }
                free(appname);
            }
        }
        if (confd_action_reply_completion(uinfo, cmplt, n) < 0) {
            opal_output(0, "%s COMPLETION FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            ret = FALSE;
        }
    } else if (0 == strcmp(options[nopts-1], "vpid")) {
        /* the job name or job-id is in the third position */
        if (0 == strcmp(options[2], "job-name")) {
            /* find this job name */
            jdata = NULL;
            for (i=1; i < orte_job_data->size; i++) {
                if (NULL == (jdt = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
                    continue;
                }
                if (NULL == jdt->name) {
                    continue;
                }
                if (0 == strcmp(jdt->name, options[3])) {
                    jdata = jdt;
                    break;
                }
            }
            if (NULL == jdata) {
                opal_output(0, "%s COMPLETION ERROR: JOB %s NOT FOUND",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            options[3]);
                ret = FALSE;
                goto cleanup;
            }
        } else if (0 == strcmp(options[2], "job-id")) {
            /* find this job number - need extra protection here as
             * confd does -not- check types on input data! Thus, a user
             * could have provided a string, or have a typo character
             * in the middle of the number
             */
            i = strtol(options[3], &remainder, 10);
            if (NULL != remainder && 0 < strlen(remainder)) {
                /* there was indeed a character in here! */
                opal_output(0, "%s COMPLETION ERROR: JOB-ID %s HAS NON-NUMERIC CHARACTER %s(%d)",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), options[3], remainder, (int)strlen(remainder));
                ret = FALSE;
                goto cleanup;
            }
            if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
                opal_output(0, "%s COMPLETION ERROR: JOB %s NOT FOUND",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            options[3]);
                ret = FALSE;
                goto cleanup;
            }
        } else {
            /* unrecognized */
            opal_output(0, "%s COMPLETION ERROR: UNRECOGNIZED PATH ELEMENT %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        options[2]);
            ret = FALSE;
            goto cleanup;
        }
        /* were we given an executable too? */
        if (5 == nopts) {
            /* nope - allocate the completion array - this is the largest the array of completions
             * can be. We may not fill it all, so we track the actual number of entries
             * and pass that value back to confd
             */
            cmplt = (struct confd_completion_value *)calloc(jdata->num_procs, sizeof(struct confd_completion_value));
            /* were we given a partial? */
            if (0 == strlen(token)) {
                /* nope - return all vpids in job */
                for (i=0, n=0; i < jdata->procs->size; i++) {
                    if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, i))) {
                        continue;
                    }
                    cmplt[n].type = CONFD_COMPLETION;
                    asprintf(&cmplt[n].value, "%s", ORTE_VPID_PRINT(proc->name.vpid));
                    cmplt[n].extra = NULL;
                    n++;
                }
            } else {
                /* return all values that start with provided token. This is
                 * a little ugly as the token is a string and the job number
                 * is an integer - hopefully, someone will devise a more
                 * efficient method!
                 */
                for (i=0, n=0; i < jdata->procs->size; i++) {
                    if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, i))) {
                        continue;
                    }
                    asprintf(&numid, "%d", proc->name.vpid);
                    if (0 == strncmp(numid, token, strlen(token))) {
                        cmplt[n].type = CONFD_COMPLETION;
                        cmplt[n].value = strdup(numid);
                        cmplt[n].extra = NULL;
                        n++;
                    }
                    free(numid);
                }
            }
        } else {
            /* find the executable */
            app = NULL;
            for (i=0; i < jdata->apps->size; i++) {
                if (NULL == (aptr = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, i))) {
                    continue;
                }
                appname = opal_basename(aptr->app);
                if (0 == strcmp(appname, options[5])) {
                    app = aptr;
                    free(appname);
                    break;
                }
                free(appname);
            }
            if (NULL == app) {
                opal_output(0, "%s COMPLETION ERROR: APP %s NOT FOUND",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            options[5]);
                ret = FALSE;
                goto cleanup;
            }
            /* allocate the completion array - this is the largest the array of completions
             * can be. We may not fill it all, so we track the actual number of entries
             * and pass that value back to confd
             */
            cmplt = (struct confd_completion_value *)calloc(app->num_procs, sizeof(struct confd_completion_value));
            /* were we given a partial? */
            if (0 == strlen(token)) {
                /* return all vpid values */
                for (i=0, n=0; i < jdata->procs->size; i++) {
                    if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, i))) {
                        continue;
                    }
                    if (proc->app_idx != app->idx) {
                        continue;
                    }
                    cmplt[n].type = CONFD_COMPLETION;
                    asprintf(&cmplt[n].value, "%s", ORTE_VPID_PRINT(proc->name.vpid));
                    cmplt[n].extra = NULL;
                    n++;
                }
            } else {
                /* return all values that start with provided token. This is
                 * a little ugly as the token is a string and the job number
                 * is an integer - hopefully, someone will devise a more
                 * efficient method!
                 */
                for (i=0, n=0; i < jdata->procs->size; i++) {
                    if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, i))) {
                        continue;
                    }
                    if (proc->app_idx != app->idx) {
                        continue;
                    }
                    asprintf(&numid, "%d", proc->name.vpid);
                    if (0 == strncmp(numid, token, strlen(token))) {
                        cmplt[n].type = CONFD_COMPLETION;
                        cmplt[n].value = strdup(numid);
                        cmplt[n].extra = NULL;
                        n++;
                    }
                    free(numid);
                }
            }
        }
        if (confd_action_reply_completion(uinfo, cmplt, n) < 0) {
            opal_output(0, "%s COMPLETION FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            ret = FALSE;
        }
    } else {
        /* unrecognized */
        opal_output(0, "%s COMPLETION ERROR: UNRECOGNIZED PARAMS %s",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), cmdpath);
    }

 cleanup:
    if (NULL != cmplt) {
        for (i=0; i < n; i++) {
            if (NULL != cmplt[i].value) {
                free(cmplt[i].value);
            }
        }
        free(cmplt);
    }
    opal_argv_free(options);
    return ret;
}

static boolean show_installed_sw(int maapisock,
                                 struct confd_user_info *uinfo,
                                 int argc, char **argv, long which)
{
    int i;
    orcm_cfgi_app_t *app;
    char *output, *result=NULL, *tmp=NULL;

    /* take control */
    ORTE_ACQUIRE_THREAD(&orcm_cfgi_base.ctl);

    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                         "%s CMDPOINT SHOW_INSTALLED_APPS CALLED",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));

    /* create a string that contains ALL of the output, and then send it
     * back to the socket - must be \n terminated!
     */
    for (i=0; i < orcm_cfgi_base.installed_apps.size; i++) {
        if (NULL == (app = (orcm_cfgi_app_t*)opal_pointer_array_get_item(&orcm_cfgi_base.installed_apps, i))) {
            continue;
        }
        orcm_cfgi_base_dump(&output, NULL, app, ORCM_CFGI_APP);
        if (NULL == result) {
            asprintf(&result, "%s\n", output);
        } else {
            asprintf(&tmp, "%s\n%s\n", result, output);
            free(result);
            result = tmp;
            tmp = NULL;
        }
        free(output);
    }

    if (NULL == result) {
        result = strdup("\n");
    }
    maapi_cli_write(maapisock, uinfo->usid, result, strlen(result));
    free(result);
    if (NULL != tmp) {
        free(tmp);
    }

    /* release control */
    ORTE_RELEASE_THREAD(&orcm_cfgi_base.ctl);

    return TRUE;
}

static boolean show_configured_sw(int maapisock,
                                  struct confd_user_info *uinfo,
                                  int argc, char **argv, long which)
{
    int i;
    orcm_cfgi_run_t *run;
    char *output, *result=NULL, *tmp=NULL;

    /* take control */
    ORTE_ACQUIRE_THREAD(&orcm_cfgi_base.ctl);

    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                         "%s CMDPOINT SHOW_CONFIGURED_APPS CALLED",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));

    /* create a string that contains ALL of the output, and then send it
     * back to the socket - must be \n terminated!
     */
    for (i=0; i < orcm_cfgi_base.confgd_apps.size; i++) {
        if (NULL == (run = (orcm_cfgi_run_t*)opal_pointer_array_get_item(&orcm_cfgi_base.confgd_apps, i))) {
            continue;
        }
        orcm_cfgi_base_dump(&output, NULL, run, ORCM_CFGI_RUN);
        if (NULL == result) {
            asprintf(&result, "%s\n", output);
        } else {
            asprintf(&tmp, "%s\n%s\n", result, output);
            free(result);
            result = tmp;
            tmp = NULL;
        }
        free(output);
    }

    if (NULL == result) {
        result = strdup("\n");
    }
    maapi_cli_write(maapisock, uinfo->usid, result, strlen(result));
    free(result);
    if (NULL != tmp) {
        free(tmp);
    }

    /* release control */
    ORTE_RELEASE_THREAD(&orcm_cfgi_base.ctl);

    return TRUE;
}

static orcm_cfgi_app_t* find_aptr(char *appname)
{
    orcm_cfgi_app_t *app, *aptr;
    int i;

    app = NULL;
    for (i=0; i < orcm_cfgi_base.installed_apps.size; i++) {
        if (NULL == (aptr = (orcm_cfgi_app_t*)opal_pointer_array_get_item(&orcm_cfgi_base.installed_apps, i))) {
            continue;
        }
        if (NULL == aptr->application) {
            continue;
        }
        if (0 == strcmp(aptr->application, appname)) {
            app = aptr;
            break;
        }
    }

    return app;
}

static orcm_cfgi_exec_t* find_eptr(orcm_cfgi_app_t *app, char *appname)
{
    orcm_cfgi_exec_t *exec, *eptr;
    int i;

    exec = NULL;
    for (i=0; i < app->executables.size; i++) {
        if (NULL == (eptr = (orcm_cfgi_exec_t*)opal_pointer_array_get_item(&app->executables, i))) {
            continue;
        }
        if (NULL == eptr->appname) {
            continue;
        }
        if (0 == strcmp(eptr->appname, appname)) {
            exec = eptr;
            break;
        }
    }

    return exec;
}

static boolean run_config_completion(struct confd_user_info *uinfo,
                                     int                     cli_style,
                                     char                   *token,
                                     int                     comp_char,
                                     confd_hkeypath_t       *kp,
                                     char                   *cmdpath,
                                     char                   *param_id)
{
    struct confd_completion_value *cmplt=NULL, value;
    int i, n, icount;
    boolean ret=TRUE;
    orcm_cfgi_app_t *app;
    orcm_cfgi_exec_t *exec;
    orcm_cfgi_version_t *vers;
    orcm_cfgi_run_t *run;
    confd_value_t *vp;
    char *application, *instance, *executable, *version;

    /* take control */
    ORTE_ACQUIRE_THREAD(&orcm_cfgi_base.ctl);

    switch(qc_get_xmltag(kp,1)) {
    case orcm_app:
        /* count the number of installed apps */
        n=0;
        for (i=0; i < orcm_cfgi_base.installed_apps.size; i++) {
            if (NULL == (app = (orcm_cfgi_app_t*)opal_pointer_array_get_item(&orcm_cfgi_base.installed_apps, i))) {
                continue;
            }
            n++;
        }

        if (0 == n) {
            /* no installed apps */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "No installed apps";
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED - NO INSTALLED APPS", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            goto depart;
        }

        /* allocate the completion array - this is the largest the array of completions
         * can be. We may not fill it all, so we track the actual number of entries
         * and pass that value back to confd
         */
        cmplt = (struct confd_completion_value *)calloc(n, sizeof(struct confd_completion_value));

        /* were we given a partial? */
        vp = qc_find_key(kp, orcm_app, 0);
        if (NULL == vp) {
            /* nope - send back all the apps */
            for (i=0, n=0; i < orcm_cfgi_base.installed_apps.size; i++) {
                if (NULL == (app = (orcm_cfgi_app_t*)opal_pointer_array_get_item(&orcm_cfgi_base.installed_apps, i))) {
                    continue;
                }
                if (NULL == app->application) {
                    continue;
                }
                cmplt[n].type = CONFD_COMPLETION;
                cmplt[n].value = strdup(app->application);
                cmplt[n].extra = NULL;
                n++;
            }
        } else {
            /* yes - just insert those that start with token */
            application = CONFD_GET_CBUFPTR(vp);
            for (i=0, n=0; i < orcm_cfgi_base.installed_apps.size; i++) {
                if (NULL == (app = (orcm_cfgi_app_t*)opal_pointer_array_get_item(&orcm_cfgi_base.installed_apps, i))) {
                    continue;
                }
                if (NULL == app->application) {
                    continue;
                }
                if (0 == strncmp(app->application, application, strlen(application))) {
                    cmplt[n].type = CONFD_COMPLETION;
                    cmplt[n].value = strdup(app->application);
                    cmplt[n].extra = NULL;
                    n++;
                }
            }
        }
        if (confd_action_reply_completion(uinfo, cmplt, n) < 0) {
            opal_output(0, "%s COMPLETION FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            ret = FALSE;
        }
        break;

    case orcm_app_instance:
        /* get the app name */
        vp = qc_find_key(kp, orcm_app, 0);
        if (NULL == vp) {
            /* can't complete */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "No app name - cannot complete";
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED - NO APPLICATION NAME", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            goto depart;
        }            
        application = CONFD_GET_CBUFPTR(vp);
        /* search the configured apps and count the matches */
        n=0;
        for (i=0; i < orcm_cfgi_base.confgd_apps.size; i++) {
            if (NULL == (run = (orcm_cfgi_run_t*)opal_pointer_array_get_item(&orcm_cfgi_base.confgd_apps, i))) {
                continue;
            }
            if (0 == strcmp(run->application, application)) {
                n++;
            }
        }
        if (0 == n) {
            /* no matches */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "No existing instances of this app";
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED - NO MATCHING INSTANCES", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            goto depart;
        }

        /* allocate the completion array - this is the largest the array of completions
         * can be. We may not fill it all, so we track the actual number of entries
         * and pass that value back to confd
         */
        cmplt = (struct confd_completion_value *)calloc(n, sizeof(struct confd_completion_value));


        /* were we given a partial? */
        vp = qc_find_key(kp, orcm_app_instance, 0);
        if (NULL == vp) {
            /* nope - send back all the matches */
            for (i=0, n=0; i < orcm_cfgi_base.confgd_apps.size; i++) {
                if (NULL == (run = (orcm_cfgi_run_t*)opal_pointer_array_get_item(&orcm_cfgi_base.confgd_apps, i))) {
                    continue;
                }
                if (0 != strcmp(run->application, application)) {
                    continue;
                }
                cmplt[n].type = CONFD_COMPLETION;
                cmplt[n].value = strdup(run->instance);
                cmplt[n].extra = NULL;
                n++;
            }
        } else {
            /* yes - just insert those that start with token */
            instance = CONFD_GET_CBUFPTR(vp);
            for (i=0, n=0; i < orcm_cfgi_base.confgd_apps.size; i++) {
                if (NULL == (run = (orcm_cfgi_run_t*)opal_pointer_array_get_item(&orcm_cfgi_base.confgd_apps, i))) {
                    continue;
                }
                if (0 != strcmp(run->application, application)) {
                    continue;
                }
                if (0 == strncmp(run->instance, instance, strlen(instance))) {
                    cmplt[n].type = CONFD_COMPLETION;
                    cmplt[n].value = strdup(run->instance);
                    cmplt[n].extra = NULL;
                    n++;
                }
            }
        }
        if (confd_action_reply_completion(uinfo, cmplt, n) < 0) {
            opal_output(0, "%s COMPLETION FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            ret = FALSE;
        }
        break;

    case orcm_exec:
        vp = qc_find_key(kp, orcm_app, 0);
        if (NULL == vp) {
            /* can't complete */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "No app name - cannot complete";
            opal_output(0, "NO APP NAME");
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED - NO APPLICATION NAME", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            goto depart;
        }
        application = CONFD_GET_CBUFPTR(vp);

        /* find this application */
        if (NULL == (app = find_aptr(application))) {
            /* unknown app */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "Unknown app";
            opal_output(0, "UNKNOWN APP %s", application);
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            goto depart;
        }

        /* count the number of installed executables */
        n=0;
        for (i=0; i < app->executables.size; i++) {
            if (NULL == (exec = (orcm_cfgi_exec_t*)opal_pointer_array_get_item(&app->executables, i))) {
                continue;
            }
            n++;
        }

        if (0 == n) {
            /* no installed executables */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "No installed executables";
            opal_output(0, "NO INSTALLED EXECUTABLES");
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED - NO INSTALLED EXECUTABLES", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            goto depart;
        }

        /* allocate the completion array - this is the largest the array of completions
         * can be. We may not fill it all, so we track the actual number of entries
         * and pass that value back to confd
         */
        cmplt = (struct confd_completion_value *)calloc(n, sizeof(struct confd_completion_value));

        /* were we given a partial? */
        vp = qc_find_key(kp, orcm_exec, 0);
        if (NULL == vp) {
            /* nope - send back all the execs */
            for (i=0, n=0; i < app->executables.size; i++) {
                if (NULL == (exec = (orcm_cfgi_exec_t*)opal_pointer_array_get_item(&app->executables, i))) {
                    continue;
                }
                if (NULL == exec->appname) {
                    continue;
                }
                cmplt[n].type = CONFD_COMPLETION;
                cmplt[n].value = strdup(exec->appname);
                cmplt[n].extra = NULL;
                n++;
            }
        } else {
            /* yes - just insert those that start with token */
            executable = CONFD_GET_CBUFPTR(vp);
            for (i=0, n=0; i < app->executables.size; i++) {
                if (NULL == (exec = (orcm_cfgi_exec_t*)opal_pointer_array_get_item(&app->executables, i))) {
                    continue;
                }
                if (NULL == exec->appname) {
                    continue;
                }
                if (0 == strncmp(exec->appname, executable, strlen(executable))) {
                    cmplt[n].type = CONFD_COMPLETION;
                    cmplt[n].value = strdup(exec->appname);
                    cmplt[n].extra = NULL;
                    n++;
                }
            }
        }
        if (confd_action_reply_completion(uinfo, cmplt, n) < 0) {
            opal_output(0, "%s COMPLETION FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            ret = FALSE;
        }
        break;

    case orcm_version:
        vp = qc_find_key(kp, orcm_app, 0);
        if (NULL == vp) {
            /* can't complete */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "No app name - cannot complete";
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED - NO APPLICATION NAME", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            goto depart;
        }
        application = CONFD_GET_CBUFPTR(vp);
        if (NULL == (app = find_aptr(application))) {
            /* unknown app */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "Unknown app";
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            goto depart;
        }
        vp = qc_find_key(kp, orcm_exec, 0);
        if (NULL == vp) {
            /* can't complete */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "No executable - cannot complete";
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED - NO EXECUTABLE", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            goto depart;
        }
        executable = CONFD_GET_CBUFPTR(vp);
        if (NULL == (exec = find_eptr(app, executable))) {
            /* unknown app */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "Unknown executable";
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            goto depart;
        }
        /* count the number of installed versions */
        n=0;
        for (i=0; i < exec->versions.size; i++) {
            if (NULL == (vers = (orcm_cfgi_version_t*)opal_pointer_array_get_item(&exec->versions, i))) {
                continue;
            }
            n++;
        }

        if (0 == n) {
            /* no installed versions */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "No installed versions";
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED - NO INSTALLED VERSIONS", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            goto depart;
        }

        /* allocate the completion array - this is the largest the array of completions
         * can be. We may not fill it all, so we track the actual number of entries
         * and pass that value back to confd
         */
        cmplt = (struct confd_completion_value *)calloc(n, sizeof(struct confd_completion_value));

        /* were we given a partial? */
        vp = qc_find_key(kp, orcm_version, 0);
        if (NULL == vp) {
            /* nope - send back all the versions */
            for (i=0, n=0; i < exec->versions.size; i++) {
                if (NULL == (vers = (orcm_cfgi_version_t*)opal_pointer_array_get_item(&exec->versions, i))) {
                    continue;
                }
                if (NULL == vers->version) {
                    continue;
                }
                cmplt[n].type = CONFD_COMPLETION;
                cmplt[n].value = strdup(vers->version);
                cmplt[n].extra = NULL;
                n++;
            }
        } else {
            /* yes - just insert those that start with token */
            version = CONFD_GET_CBUFPTR(vp);
            for (i=0, n=0; i < exec->versions.size; i++) {
                if (NULL == (vers = (orcm_cfgi_version_t*)opal_pointer_array_get_item(&exec->versions, i))) {
                    continue;
                }
                if (NULL == vers->version) {
                    continue;
                }
                if (0 == strncmp(vers->version, version, strlen(version))) {
                    cmplt[n].type = CONFD_COMPLETION;
                    cmplt[n].value = strdup(vers->version);
                    cmplt[n].extra = NULL;
                    n++;
                }
            }
        }
        if (confd_action_reply_completion(uinfo, cmplt, n) < 0) {
            opal_output(0, "%s COMPLETION FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            ret = FALSE;
        }
        break;

    case orcm_count:
        vp = qc_find_key(kp, orcm_app, 0);
        if (NULL == vp) {
            /* can't complete */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "No app name - cannot complete";
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED - NO APPLICATION NAME", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            goto depart;
        }
        application = CONFD_GET_CBUFPTR(vp);
        if (NULL == (app = find_aptr(application))) {
            /* unknown app */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "Unknown app";
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            goto depart;
        }
        vp = qc_find_key(kp, orcm_exec, 0);
        if (NULL == vp) {
            /* can't complete */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "No executable - cannot complete";
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED - NO EXECUTABLE", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            goto depart;
        }
        executable = CONFD_GET_CBUFPTR(vp);
        if (NULL == (exec = find_eptr(app, executable))) {
            /* unknown app */
            value.type = CONFD_COMPLETION_INFO;
            value.value = "Unknown executable";
            if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
                opal_output(0, "%s COMPLETION FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                ret = FALSE;
            }
            goto depart;
        }
        /* allocate the completion array */
        cmplt = (struct confd_completion_value *)calloc(2, sizeof(struct confd_completion_value));
        /* if the range is unlimited, respond accordingly */
        if (exec->process_limit < 0) {
            cmplt[0].type = CONFD_COMPLETION;
            cmplt[0].value = strdup("1...");
            cmplt[0].extra = NULL;
            cmplt[1].type = CONFD_COMPLETION;
            cmplt[1].value = strdup("N");
            cmplt[1].extra = NULL;
        } else {
            /* no - show the allowed range */
            cmplt[0].type = CONFD_COMPLETION;
            cmplt[0].value = strdup("1...");
            cmplt[0].extra = NULL;
            cmplt[1].type = CONFD_COMPLETION;
            asprintf(&cmplt[1].value, "%d", exec->process_limit);
            cmplt[1].extra = NULL;
        }

        if (confd_action_reply_completion(uinfo, cmplt, 2) < 0) {
            opal_output(0, "%s COMPLETION FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            ret = FALSE;
        }
        break;

    default:
        /* unrecognized */
        opal_output(0, "%s COMPLETION ERROR: UNRECOGNIZED PARAMS %s",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), cmdpath);
        value.type = CONFD_COMPLETION_INFO;
        value.value = "UNRECOGNIZED PARAM";
        if (confd_action_reply_completion(uinfo, &value, 1) < 0) {
            opal_output(0, "%s COMPLETION FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            ret = FALSE;
        }
    }

 depart:
    /* release control */
    ORTE_RELEASE_THREAD(&orcm_cfgi_base.ctl);

    return ret;
}


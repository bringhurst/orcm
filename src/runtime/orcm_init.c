/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config_private.h"
#include "include/constants.h"

#include <signal.h>

#include "opal/util/error.h"

#include "orte/runtime/runtime.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/plm/plm.h"
#include "orte/mca/grpcomm/grpcomm.h"
#include "orte/runtime/orte_locks.h"

#include "mca/pnp/base/public.h"
#include "runtime/runtime.h"

const char openrcm_version_string[] = "OPENRCM 0.1";
bool orcm_initialized = false;
bool orcm_util_initialized = false;
bool orcm_finalizing = false;
int orcm_debug_output = -1;
int orcm_debug_verbosity = 0;

/* signal trap support */
/* available signals
    SIGHUP,
    SIGINT,     <=== trapped
    SIGQUIT,    <=== trapped
    SIGILL,
    SIGTRAP,
    SIGABRT,
    SIGFPE,
    SIGBUS,
    SIGSEGV,
    SIGSYS,
    SIGPIPE,    <=== trapped, ignored
    SIGTERM     <=== trapped
*/

static int signals[] = {
    SIGTERM,
    SIGINT,
    SIGQUIT,
    SIGPIPE
};
static struct opal_event trap_handler[4];
static int num_signals=4;

static void trap_signals(void);
static void signal_trap(int fd, short flags, void *arg);
static void ignore_trap(int fd, short flags, void *arg);
static bool forcibly_die=false;

int orcm_init(orcm_proc_type_t flags)
{
    int ret;
    char *error;
    int spin;
    
    if (NULL != getenv("ORCM_MCA_spin")) {
        spin = 1;
        /* spin until a debugger can attach */
        while (0 != spin) {
            ret = 0;
            while (ret < 10000) {
                ret++;
            };
        }
    }
    
    if (!orcm_util_initialized) {
        orcm_init_util();
    }
    
    /* set some envars generally needed */
    putenv("OMPI_MCA_routed=cm");
    putenv("OMPI_MCA_orte_create_session_dirs=0");
    
    if (OPENRCM_MASTER & flags) {
        /* add envars the master needs */
        if (NULL == getenv("OMPI_MCA_rmaps")) {
            putenv("OMPI_MCA_rmaps=resilient");
        }
        
        /* if we are the master, then init us
         * with ORTE as the HNP
         */
        if (ORTE_SUCCESS != (ret = orte_init(NULL, NULL, ORTE_PROC_HNP))) {
            error = "orte_init";
            goto error;
        }
        
    }  else if (OPENRCM_DAEMON & flags) {
        /* ensure we use the right ess module if one isn't given */
        if (NULL == getenv("OMPI_MCA_ess=cm")) {
            putenv("OMPI_MCA_ess=cm");
        }
        if (ORTE_SUCCESS != (ret = orte_init(NULL, NULL, ORTE_PROC_DAEMON))) {
            error = "orte_init";
            goto error;
        }
        
    } else if (OPENRCM_TOOL & flags) {
        /* tools start independently, so we have to
         * ensure they get the correct ess module
         */
        if (NULL == getenv("OMPI_MCA_ess=cm")) {
            putenv("OMPI_MCA_ess=cm");
        }
        if (ORTE_SUCCESS != (ret = orte_init(NULL, NULL, ORTE_PROC_TOOL))) {
            error = "orte_init";
            goto error;
        }
        
    } else if (OPENRCM_APP & flags) {
        /* apps are always started by the daemon, so
         * they will be told the right components to open
         */
        if (ORTE_SUCCESS != (ret = orte_init(NULL, NULL, ORTE_PROC_NON_MPI))) {
            error = "orte_init";
            goto error;
        }
        
    } else {
        error = "unknown flag";
        ret = ORTE_ERR_FATAL;
        goto error;
    }

    /* setup the pnp framework */
    if (ORCM_SUCCESS != (ret = orcm_pnp_base_open())) {
        error = "pnp_open";
        goto error;
    }
    if (ORCM_SUCCESS != (ret = orcm_pnp_base_select())) {
        error = "pnp_select";
        goto error;
    }
    /* setup the leader framework */
    if (ORCM_SUCCESS != (ret = orcm_leader_base_open())) {
        error = "pnp_open";
        goto error;
    }
    if (ORCM_SUCCESS != (ret = orcm_leader_base_select())) {
        error = "pnp_select";
        goto error;
    }

    trap_signals();

    orcm_initialized = true;
    
    return ORCM_SUCCESS;

error:
    if (ORCM_ERR_SILENT != ret) {
        orte_show_help("help-openrcm-runtime.txt",
                       "orcm_init:startup:internal-failure",
                       true, error, ORTE_ERROR_NAME(ret), ret);
    }
    
    return ret;
}

int orcm_init_util(void)
{
    int ret;
    char *error;
    
    /* Ensure that enough of OPAL is setup for us to be able to run */
    if( ORTE_SUCCESS != (ret = opal_init_util(NULL, NULL)) ) {
        error = "opal_init_util";
        goto error;
    }
    /* register handler for errnum -> string conversion */
    opal_error_register("OPENRCM", ORCM_ERR_BASE, ORCM_ERR_MAX, orcm_err2str);
    /* register where the OPENRCM show_help files are located */
    if (ORTE_SUCCESS != (ret = opal_show_help_add_dir(OPENRCM_HELPFILES))) {
        error = "register show_help_dir";
    goto error;
    }
    
    orcm_util_initialized = true;
    
    return ORCM_SUCCESS;
    
error:
    if (ORCM_ERR_SILENT != ret) {
        orte_show_help("help-openrcm-runtime.txt",
                       "orcm_init:startup:internal-failure",
                       true, error, ORTE_ERROR_NAME(ret), ret);
    }
    
    return ret;
}

/**   SIGNAL TRAP    **/
void orcm_remove_signal_handlers(void)
{
    int i;
    
    for (i=0; i < num_signals; i++) {
        opal_signal_del(&trap_handler[i]);
    }
}

static void trap_signals(void)
{
    int i;
    
    for (i=0; i < num_signals; i++) {
        if (SIGPIPE == signals[i]) {
            /* ignore this signal */
            opal_signal_set(&trap_handler[i], signals[i],
                            ignore_trap, &trap_handler[i]);
        } else {
            opal_signal_set(&trap_handler[i], signals[i],
                            signal_trap, &trap_handler[i]);
        }
        opal_signal_add(&trap_handler[i], NULL);
    }
}

static void just_quit(int fd, short flags, void*arg)
{

    if (OPENRCM_PROC_IS_APP || OPENRCM_PROC_IS_TOOL) {
        /* whack any lingering session directory files from our job */
        orte_session_dir_cleanup(ORTE_PROC_MY_NAME->jobid);
    } else {
        /* whack any lingering session directory files from our jobs */
        orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
    }
    
    /* cleanup and leave */
    orcm_finalize();
    
    exit(orte_exit_status);
}

static void abort_callback(int fd, short flags, void*arg)
{
    int j;
    orte_job_t *jdata;
    int ret;
    
    /* since we are being terminated by a user's signal, be
     * sure to exit with a non-zero exit code - but don't
     * overwrite any error code from a proc that might have
     * failed, in case that is why the user ordered us
     * to terminate
     */
    ORTE_UPDATE_EXIT_STATUS(ORTE_ERROR_DEFAULT_EXIT_CODE);
    
    for (j=0; j < orte_job_data->size; j++) {
        if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, j))) {
            continue;
        }
        /* turn off the restart for this job */
        jdata->err_cbfunc = NULL;
        jdata->err_cbstates = 0;
        /* indicate that this is no longer a continuously operating job */
        jdata->controls &= ~ORTE_JOB_CONTROL_CONTINUOUS_OP;
    }
    
    /* terminate the orteds - they will automatically kill
     * their local procs
     */
    ret = orte_plm.terminate_orteds();
    if (ORTE_SUCCESS != ret) {
        /* If we failed the terminate_orteds() above, then we
         * need to just die
         */
        just_quit(0, 0, NULL);
    }
    /* ensure all the orteds depart together */
    orte_grpcomm.onesided_barrier();
    
    /* all done - time to go */
    just_quit(0, 0, NULL);
}

static void signal_trap(int fd, short flags, void *arg)
{
    int i;

    opal_output(0, "%s trapped signal %s",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), strsignal(fd));

    /* We are in an event handler; the exit procedure
     * will delete the signal handler that is currently running
     * (which is a Bad Thing), so we can't call it directly.
     * Instead, we have to exit this handler and setup to call
     * exit after this.
     */
    /* if we are an app, just cleanly terminate */
    if (OPENRCM_PROC_IS_APP || OPENRCM_PROC_IS_TOOL) {
        if (!opal_atomic_trylock(&orte_abort_inprogress_lock)) { /* returns 1 if already locked */
            return;
        }
        ORTE_TIMER_EVENT(0, 0, just_quit);
        return;
    }
    
    /* if we are a daemon or HNP, allow for a forced term */
    if (!opal_atomic_trylock(&orte_abort_inprogress_lock)) { /* returns 1 if already locked */
        if (forcibly_die) {
            opal_output(0, "%s forcibly exiting upon signal %s",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), strsignal(fd));
            
            /* kill any local procs */
            orte_odls.kill_local_procs(NULL);
            
            /* whack any lingering session directory files from our jobs */
            orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
            /* exit with a non-zero status */
            exit(ORTE_ERROR_DEFAULT_EXIT_CODE);
        }
        opal_output(0, "orcm: abort is already in progress...hit ctrl-c again to forcibly terminate\n\n");
        forcibly_die = true;
        return;
    }
    
    /* set the global abnormal exit flag so we know not to
     * use the standard xcast for terminating orteds
     */
    orte_abnormal_term_ordered = true;
    /* ensure that the forwarding of stdin stops */
    orte_job_term_ordered = true;
    
    ORTE_TIMER_EVENT(0, 0, abort_callback);
 }

static void ignore_trap(int fd, short flags, void *arg)
{
    opal_output(0, "%s ignoring signal %s",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), strsignal(fd));
    
    return;
}

/**   INSTANTIATE OPENRCM OBJECTS **/
static void spawn_construct(orcm_spawn_event_t *ptr)
{
    ptr->ev = (opal_event_t*)malloc(sizeof(opal_event_t));
    ptr->cmd = NULL;
    ptr->np = 0;
    ptr->hosts = NULL;
    ptr->constrain = false;
    ptr->add_procs = false;
    ptr->debug = false;
}
static void spawn_destruct(orcm_spawn_event_t *ptr)
{
    if (NULL != ptr->ev) { 
        free(ptr->ev); 
    } 
    if (NULL != ptr->cmd) {
        free(ptr->cmd);
    }
    if (NULL != ptr->hosts) {
        free(ptr->hosts);
    }
}
OBJ_CLASS_INSTANCE(orcm_spawn_event_t,
                   opal_object_t,
                   spawn_construct,
                   spawn_destruct);


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
bool orcm_lowest_rank = false;

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
    
    if (ORTE_SUCCESS != (ret = orte_init(NULL, NULL, flags))) {
        error = "orte_init";
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
    int ret, i;
    char *error;
    char *destdir, *tmp, *mcp, *new_mcp;

    /* Setup OPAL */
    if( ORTE_SUCCESS != (ret = opal_init(NULL, NULL)) ) {
        error = "opal_init_util";
        goto error;
    }
    /* register handler for errnum -> string conversion */
    opal_error_register("OPENRCM", ORCM_ERR_BASE, ORCM_ERR_MAX, orcm_err2str);
    /* register where the OPENRCM show_help files are located */
    if (NULL != (destdir = getenv("ORCM_DESTDIR"))) {
        asprintf(&tmp, "%s%s", destdir, ORCM_PKGHELPDIR);
    } else {
        tmp = strdup(ORCM_PKGHELPDIR);
    }
    if (ORTE_SUCCESS != (ret = opal_show_help_add_dir(tmp))) {
        error = "register show_help_dir";
        goto error;
    }
    free(tmp);
    
    /* Add ORCM's component directory into the
       mca_base_param_component_path */
    i = mca_base_param_find("mca", NULL, "component_path");
    if (i < 0) {
        ret = ORCM_ERR_NOT_FOUND;
        error = "Could not find mca_component_path";
        goto error;
    }
    mca_base_param_lookup_string(i, &mcp);
    if (NULL == mcp) {
        ret = ORCM_ERR_NOT_FOUND;
        error = "Could not find mca_component_path";
        goto error;
    }
    if (NULL != destdir) {
        asprintf(&new_mcp, "%s%s:%s", destdir, ORCM_PKGLIBDIR, mcp);
    } else {
        asprintf(&new_mcp, "%s:%s", ORCM_PKGLIBDIR, mcp);
    }
    mca_base_param_set_string(i, new_mcp);
    free(new_mcp);

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

    if (ORCM_PROC_IS_APP || ORCM_PROC_IS_TOOL) {
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

static void signal_trap(int fd, short flags, void *arg)
{
    int i;

    /* We are in an event handler; the exit procedure
     * will delete the signal handler that is currently running
     * (which is a Bad Thing), so we can't call it directly.
     * Instead, we have to exit this handler and setup to call
     * exit after this.
     */
    /* if we are an app, just cleanly terminate */
    if (ORCM_PROC_IS_APP || ORCM_PROC_IS_TOOL) {
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
    
    ORTE_TIMER_EVENT(0, 0, just_quit);
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
    ptr->continuous = false;
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


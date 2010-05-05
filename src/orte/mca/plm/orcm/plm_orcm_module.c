/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2007 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2006 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2007 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2007      Los Alamos National Security, LLC.  All rights
 *                         reserved. 
 * Copyright (c) 2008-2010 Sun Microsystems, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "openrcm_config_private.h"
#include "include/constants.h"

#include "orte/constants.h"

#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#include <fcntl.h>
#include <signal.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif

#include "opal/mca/installdirs/installdirs.h"
#include "opal/mca/base/mca_base_param.h"
#include "opal/util/output.h"
#include "opal/event/event.h"
#include "opal/util/argv.h"
#include "opal/util/path.h"
#include "opal/util/opal_environ.h"
#include "opal/util/basename.h"
#include "opal/util/bit_ops.h"
#include "opal/class/opal_pointer_array.h"
#include "opal/threads/threads.h"

#include "orte/util/show_help.h"
#include "orte/runtime/orte_wait.h"
#include "orte/runtime/orte_globals.h"
#include "orte/util/name_fns.h"
#include "orte/util/nidmap.h"
#include "orte/util/proc_info.h"

#include "orte/mca/rml/rml.h"
#include "orte/mca/rml/rml_types.h"
#include "orte/mca/ess/ess.h"
#include "orte/mca/ess/base/base.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rmaps/rmaps.h"
#include "orte/mca/routed/routed.h"
#include "orte/mca/rml/base/rml_contact.h"

#include "orte/mca/plm/plm.h"
#include "orte/mca/plm/base/base.h"
#include "orte/mca/plm/base/plm_private.h"

#include "mca/pnp/pnp.h"

#include "plm_orcm.h"

/* API functions */

static int init(void);
static int launch(orte_job_t *jdata);
static int terminate_orteds(void);
static int signal_job(orte_jobid_t jobid, int32_t signal);
static int finalize(void);

orte_plm_base_module_t orte_plm_orcm_module = {
    init,
    orte_plm_base_set_hnp_name,
    launch,
    NULL,
    orte_plm_base_orted_terminate_job,
    terminate_orteds,
    orte_plm_base_orted_kill_local_procs,
    signal_job,
    finalize
};

typedef enum {
    ORTE_PLM_RSH_SHELL_BASH = 0,
    ORTE_PLM_RSH_SHELL_ZSH,
    ORTE_PLM_RSH_SHELL_TCSH,
    ORTE_PLM_RSH_SHELL_CSH,
    ORTE_PLM_RSH_SHELL_KSH,
    ORTE_PLM_RSH_SHELL_SH,
    ORTE_PLM_RSH_SHELL_UNKNOWN
} rsh_shell_t;

/* These strings *must* follow the same order as the enum
 ORTE_PLM_RSH_SHELL_* */
static const char *rsh_shell_name[] = {
    "bash",
    "zsh",
    "tcsh",       /* tcsh has to be first otherwise strstr finds csh */
    "csh",
    "ksh",
    "sh",
    "unknown"
};

/*
 * Local functions
 */
static void set_handler_default(int sig);
static rsh_shell_t find_shell(char *shell);
static int rsh_probe(char *nodename, rsh_shell_t *shell);
static int setup_shell(rsh_shell_t *rshell,
                       rsh_shell_t *lshell,
                       char *nodename, int *argc, char ***argv);

/* local global storage of timing variables */
static struct timeval joblaunchstart, joblaunchstop;

/* local global storage */
static orte_jobid_t active_job=ORTE_JOBID_INVALID;

/**
 * Init the module
 */
static int init(void)
{
    return ORTE_SUCCESS;
}

/**
 * Callback on daemon exit.
 */

static void wait_daemon(pid_t pid, int status, void* cbdata)
{
    orte_std_cntr_t cnt=1;
    uint8_t flag;
    orte_job_t *jdata;
    orte_proc_t *daemon = (orte_proc_t*)cbdata;
    
    if (! WIFEXITED(status) || ! WEXITSTATUS(status) == 0) { /* if abnormal exit */
        OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                             "%s daemon %d failed with status %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (int)daemon->name.vpid, WEXITSTATUS(status)));
        /* note that this daemon failed */
        daemon->state = ORTE_PROC_STATE_FAILED_TO_START;
        /* increment the #daemons terminated so we will exit properly */
        jdata->num_terminated++;
        
    }
    
    /* release any waiting threads */
    OPAL_THREAD_LOCK(&mca_plm_orcm_component.lock);
    
    if (mca_plm_orcm_component.num_children-- >=
        mca_plm_orcm_component.num_concurrent ||
        mca_plm_orcm_component.num_children == 0) {
        opal_condition_signal(&mca_plm_orcm_component.cond);
    }
    
    OPAL_THREAD_UNLOCK(&mca_plm_orcm_component.lock);
    
}

static int setup_launch(int *argcptr, char ***argvptr,
                        char *nodename,
                        int *node_name_index1,
                        int *proc_vpid_index, char *prefix_dir)
{
    int argc;
    char **argv;
    char *param;
    rsh_shell_t remote_shell, local_shell;
    char *lib_base, *bin_base;
    int orted_argc;
    char **orted_argv;
    char *orted_cmd, *orted_prefix, *final_cmd;
    int orted_index;
    int rc;

    
    /* Figure out the basenames for the libdir and bindir.  This
     requires some explanation:
     
     - Use opal_install_dirs.libdir and opal_install_dirs.bindir.
     
     - After a discussion on the devel-core mailing list, the
     developers decided that we should use the local directory
     basenames as the basis for the prefix on the remote note.
     This does not handle a few notable cases (e.g., if the
     libdir/bindir is not simply a subdir under the prefix, if the
     libdir/bindir basename is not the same on the remote node as
     it is here on the local node, etc.), but we decided that
     --prefix was meant to handle "the common case".  If you need
     something more complex than this, a) edit your shell startup
     files to set PATH/LD_LIBRARY_PATH properly on the remove
     node, or b) use some new/to-be-defined options that
     explicitly allow setting the bindir/libdir on the remote
     node.  We decided to implement these options (e.g.,
     --remote-bindir and --remote-libdir) to orterun when it
     actually becomes a problem for someone (vs. a hypothetical
     situation).
     
     Hence, for now, we simply take the basename of this install's
     libdir and bindir and use it to append this install's prefix
     and use that on the remote node.
     */
    
    lib_base = opal_basename(opal_install_dirs.libdir);
    bin_base = opal_basename(opal_install_dirs.bindir);
    
    /*
     * Build argv array
     */
    argv = opal_argv_copy(orte_plm_globals.rsh_agent_argv);
    argc = opal_argv_count(orte_plm_globals.rsh_agent_argv);
    *node_name_index1 = argc;
    opal_argv_append(&argc, &argv, "<template>");
    
    /* setup the correct shell info */
    if (ORTE_SUCCESS != (rc = setup_shell(&remote_shell, &local_shell,
                                          nodename, &argc, &argv))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* now get the orted cmd - as specified by user - into our tmp array.
     * The function returns the location where the actual orted command is
     * located - usually in the final spot, but someone could
     * have added options. For example, it should be legal for them to use
     * "orted --debug-devel" so they get debug output from the orteds, but
     * not from mpirun. Also, they may have a customized version of orted
     * that takes arguments in addition to the std ones we already support
     */
    orted_argc = 0;
    orted_argv = NULL;
    orted_index = orte_plm_base_setup_orted_cmd(&orted_argc, &orted_argv);
    
    /* look at the returned orted cmd argv to check several cases:
     *
     * - only "orted" was given. This is the default and thus most common
     *   case. In this situation, there is nothing we need to do
     *
     * - something was given that doesn't include "orted" - i.e., someone
     *   has substituted their own daemon. There isn't anything we can
     *   do here, so we want to avoid adding prefixes to the cmd
     *
     * - something was given that precedes "orted". For example, someone
     *   may have specified "valgrind [options] orted". In this case, we
     *   need to separate out that "orted_prefix" section so it can be
     *   treated separately below
     *
     * - something was given that follows "orted". An example was given above.
     *   In this case, we need to construct the effective "orted_cmd" so it
     *   can be treated properly below
     *
     * Obviously, the latter two cases can be combined - just to make it
     * even more interesting! Gotta love rsh/ssh...
     */
    if (0 == orted_index) {
        /* single word cmd - this is the default scenario, but there could
         * be options specified so we need to account for that possibility.
         * However, we don't need/want a prefix as nothing precedes the orted
         * cmd itself
         */
        orted_cmd = opal_argv_join(orted_argv, ' ');
        orted_prefix = NULL;
    } else {
        /* okay, so the "orted" cmd is somewhere in this array, with
         * something preceding it and perhaps things following it.
         */
        orted_prefix = opal_argv_join_range(orted_argv, 0, orted_index, ' ');
        orted_cmd = opal_argv_join_range(orted_argv, orted_index, opal_argv_count(orted_argv), ' ');
    }
    opal_argv_free(orted_argv);  /* done with this */
    
    /* we now need to assemble the actual cmd that will be executed - this depends
     * upon whether or not a prefix directory is being used
     */
    if (NULL != prefix_dir) {
        /* if we have a prefix directory, we need to set the PATH and
         * LD_LIBRARY_PATH on the remote node, and prepend just the orted_cmd
         * with the prefix directory
         */
        char *opal_prefix = getenv("OPAL_PREFIX");
        char* full_orted_cmd = NULL;

        if( NULL != orted_cmd ) {
            asprintf( &full_orted_cmd, "%s/%s/%s", prefix_dir, bin_base, orted_cmd );
        }

        if (ORTE_PLM_RSH_SHELL_SH == remote_shell ||
            ORTE_PLM_RSH_SHELL_KSH == remote_shell ||
            ORTE_PLM_RSH_SHELL_ZSH == remote_shell ||
            ORTE_PLM_RSH_SHELL_BASH == remote_shell) {
            /* if there is nothing preceding orted, then we can just
             * assemble the cmd with the orted_cmd at the end. Otherwise,
             * we have to insert the orted_prefix in the right place
             */
            asprintf (&final_cmd,
                      "%s%s%s PATH=%s/%s:$PATH ; export PATH ; "
                      "LD_LIBRARY_PATH=%s/%s:$LD_LIBRARY_PATH ; export LD_LIBRARY_PATH ; "
                      "%s %s",
                      (opal_prefix != NULL ? "OPAL_PREFIX=" : " "),
                      (opal_prefix != NULL ? opal_prefix : " "),
                      (opal_prefix != NULL ? " ; export OPAL_PREFIX;" : " "),
                      prefix_dir, bin_base,
                      prefix_dir, lib_base,
                      (orted_prefix != NULL ? orted_prefix : " "),
                      (full_orted_cmd != NULL ? full_orted_cmd : " "));
        } else if (ORTE_PLM_RSH_SHELL_TCSH == remote_shell ||
                   ORTE_PLM_RSH_SHELL_CSH == remote_shell) {
            /* [t]csh is a bit more challenging -- we
             have to check whether LD_LIBRARY_PATH
             is already set before we try to set it.
             Must be very careful about obeying
             [t]csh's order of evaluation and not
             using a variable before it is defined.
             See this thread for more details:
             http://www.open-mpi.org/community/lists/users/2006/01/0517.php. */
            /* if there is nothing preceding orted, then we can just
             * assemble the cmd with the orted_cmd at the end. Otherwise,
             * we have to insert the orted_prefix in the right place
             */
            asprintf (&final_cmd,
                      "%s%s%s set path = ( %s/%s $path ) ; "
                      "if ( $?LD_LIBRARY_PATH == 1 ) "
                      "set OMPI_have_llp ; "
                      "if ( $?LD_LIBRARY_PATH == 0 ) "
                      "setenv LD_LIBRARY_PATH %s/%s ; "
                      "if ( $?OMPI_have_llp == 1 ) "
                      "setenv LD_LIBRARY_PATH %s/%s:$LD_LIBRARY_PATH ; "
                      "%s %s",
                      (opal_prefix != NULL ? "setenv OPAL_PREFIX " : " "),
                      (opal_prefix != NULL ? opal_prefix : " "),
                      (opal_prefix != NULL ? " ;" : " "),
                      prefix_dir, bin_base,
                      prefix_dir, lib_base,
                      prefix_dir, lib_base,
                      (orted_prefix != NULL ? orted_prefix : " "),
                      (full_orted_cmd != NULL ? full_orted_cmd : " "));
        } else {
            orte_show_help("help-plm-orcm.txt", "cannot-resolve-shell-with-prefix", true,
                           (NULL == opal_prefix) ? "NULL" : opal_prefix,
                           prefix_dir);
            return ORTE_ERR_SILENT;
        }
        if( NULL != full_orted_cmd ) {
            free(full_orted_cmd);
        }
    } else {
        /* no prefix directory, so just aggregate the result */
        asprintf(&final_cmd, "%s %s",
                 (orted_prefix != NULL ? orted_prefix : ""),
                 (orted_cmd != NULL ? orted_cmd : ""));
    }
    /* now add the final cmd to the argv array */
    opal_argv_append(&argc, &argv, final_cmd);
    free(final_cmd);  /* done with this */
    if (NULL != orted_prefix) free(orted_prefix);
    if (NULL != orted_cmd) free(orted_cmd);
    
    /* if we are not debugging, tell the daemon
     * to daemonize so we can launch the next group
     */
    if (!orte_debug_flag &&
        !orte_debug_daemons_flag &&
        !orte_debug_daemons_file_flag &&
        !orte_leave_session_attached) {
        opal_argv_append(&argc, &argv, "--daemonize");
    }
    
    /*
     * Add the basic arguments to the orted command line, including
     * all debug options
     */
    orte_plm_base_orted_append_basic_args(&argc, &argv,
                                          "cm",
                                          proc_vpid_index,
                                          NULL);
    
    /* ensure that only the orcm plm is selected on the remote daemon */
    opal_argv_append_nosize(&argv, "-mca");
    opal_argv_append_nosize(&argv, "plm");
    opal_argv_append_nosize(&argv, "orcm");
    
    /* in the rsh environment, we can append multi-word arguments
     * by enclosing them in quotes. Check for any multi-word
     * mca params and include them
     */
    if (ORTE_PROC_IS_HNP || ORTE_PROC_IS_DAEMON) {
        int cnt, i;
        cnt = opal_argv_count(orted_cmd_line);    
        for (i=0; i < cnt; i+=3) {
            /* check if the specified option is more than one word - all
             * others have already been passed
             */
            if (NULL != strchr(orted_cmd_line[i+2], ' ')) {
                /* must add quotes around it */
                asprintf(&param, "\"%s\"", orted_cmd_line[i+2]);
                /* now pass it along */
                opal_argv_append(&argc, &argv, orted_cmd_line[i]);
                opal_argv_append(&argc, &argv, orted_cmd_line[i+1]);
                opal_argv_append(&argc, &argv, param);
                free(param);
            }
        }
    }

    if (ORTE_PLM_RSH_SHELL_SH == remote_shell ||
        ORTE_PLM_RSH_SHELL_KSH == remote_shell) {
        opal_argv_append(&argc, &argv, ")");
    }

    if (0 < opal_output_get_verbosity(orte_plm_globals.output)) {
        param = opal_argv_join(argv, ' ');
        OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                             "%s plm:orcm: final template argv:\n\t%s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (NULL == param) ? "NULL" : param));
        if (NULL != param) free(param);
    }
    
    /* all done */
    *argcptr = argc;
    *argvptr = argv;
    return ORTE_SUCCESS;
}

/* actually ssh the child */
static void ssh_child(int argc, char **argv, orte_vpid_t vpid,
                      int proc_vpid_index, int node_name_index1)
{
    char** env;
    char* var;
    long fd, fdmax = sysconf(_SC_OPEN_MAX);
    int rc;
    char *exec_path;
    char **exec_argv;
    int fdin;
    sigset_t sigs;
    char *node_name;
    char cwd[OPAL_PATH_MAX];
    size_t i;
    
    /* setup environment */
    env = opal_argv_copy(orte_launch_environ);
    
    /* We don't need to sense an oversubscribed condition and set the sched_yield
     * for the node as we are only launching the daemons at this time. The daemons
     * are now smart enough to set the oversubscribed condition themselves when
     * they launch the local procs.
     */
    
    node_name = argv[node_name_index1];
    
    /* if this is a local launch, then we just exec - we were already fork'd */
    if (0 == strcmp(node_name, orte_process_info.nodename) ||
        0 == strcmp(node_name, "localhost") ||
        opal_ifislocal(node_name)) {
        getcwd(cwd, OPAL_PATH_MAX);
        exec_argv = &argv[node_name_index1+1];
        /* sounds strange, but there is a space at the beginning of this
         * which will interfere with finding the executable, so remove it
         */
        for (i=1; i < strlen(exec_argv[0]); i++) {
            exec_argv[0][i-1] = exec_argv[0][i];
        }
        exec_argv[0][i-1] = '\0';
        exec_path = opal_path_findv(exec_argv[0], X_OK, orte_launch_environ, cwd);
        if (NULL == exec_path) {
            opal_output(0, "Cannot locate executable %s in path", exec_argv[0]);
            exit(-1);
        }
    } else {
        /* remote launch */
        exec_argv = argv;
        exec_path = strdup(orte_plm_globals.rsh_agent_path);
    }
    
    /* pass the vpid */
    rc = orte_util_convert_vpid_to_string(&var, vpid);
    if (ORTE_SUCCESS != rc) {
        opal_output(0, "orte_plm_orcm: unable to get daemon vpid as string");
        exit(-1);
    }
    free(argv[proc_vpid_index]);
    argv[proc_vpid_index] = strdup(var);
    free(var);
    
    /* Don't let ssh slurp all of our stdin! */
    fdin = open("/dev/null", O_RDWR);
    dup2(fdin, 0);
    close(fdin);
    
    /* close all file descriptors w/ exception of stdin/stdout/stderr */
    for(fd=3; fd<fdmax; fd++)
        close(fd);
    
    /* Set signal handlers back to the default.  Do this close
     to the execve() because the event library may (and likely
     will) reset them.  If we don't do this, the event
     library may have left some set that, at least on some
     OS's, don't get reset via fork() or exec().  Hence, the
     daemon could be unkillable (for example). */
    
    set_handler_default(SIGTERM);
    set_handler_default(SIGINT);
    set_handler_default(SIGHUP);
    set_handler_default(SIGPIPE);
    set_handler_default(SIGCHLD);
    
    /* Unblock all signals, for many of the same reasons that
     we set the default handlers, above.  This is noticable
     on Linux where the event library blocks SIGTERM, but we
     don't want that blocked by the orted (or, more
     specifically, we don't want it to be blocked by the
     orted and then inherited by the ORTE processes that it
     forks, making them unkillable by SIGTERM). */
    sigprocmask(0, 0, &sigs);
    sigprocmask(SIG_UNBLOCK, &sigs, 0);
    
    /* exec the daemon */
    var = opal_argv_join(exec_argv, ' ');
    OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                         "%s plm:orcm: executing: (%s) [%s]",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         exec_path, (NULL == var) ? "NULL" : var));
    if (NULL != var) free(var);
    
    execve(exec_path, exec_argv, env);
    opal_output(0, "plm:orcm: execv of %s failed with errno=%s(%d)\n",
                exec_path, strerror(errno), errno);
    exit(-1);
}

/**
 * Launch a daemon (bootproxy) on each node. The daemon will be responsible
 * for launching the application.
 */

/* When working in this function, ALWAYS jump to "cleanup" if
 * you encounter an error so that orterun will be woken up and
 * the job can cleanly terminate
 */
static int launch(orte_job_t *jdata)
{
    orte_job_map_t *map = NULL;
    int node_name_index1;
    int proc_vpid_index;
    char **argv = NULL;
    char *prefix_dir;
    int argc;
    int rc;
    bool failed_launch = true;
    orte_app_context_t *app;
    orte_node_t *node;
    orte_std_cntr_t nnode;
    orte_jobid_t failed_job;
    orte_job_state_t job_state = ORTE_JOB_STATE_NEVER_LAUNCHED;
    
    /* wait for the launch to complete */
    OPAL_ACQUIRE_THREAD(&orte_plm_globals.spawn_lock,
                        &orte_plm_globals.spawn_in_progress_cond,
                        &orte_plm_globals.spawn_in_progress);
    OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output, "released to spawn"));
    orte_plm_globals.spawn_in_progress = true;
    OPAL_THREAD_UNLOCK(&orte_plm_globals.spawn_lock);
    
    /* if we are timing, record the start time */
    if (orte_timing) {
        gettimeofday(&orte_plm_globals.daemonlaunchstart, NULL);
    }

    /* default to declaring the daemon launch as having failed */
    failed_job = ORTE_PROC_MY_NAME->jobid;
    
    if (orte_timing) {
        if (0 != gettimeofday(&joblaunchstart, NULL)) {
            opal_output(0, "plm_orcm: could not obtain start time");
            joblaunchstart.tv_sec = 0;
            joblaunchstart.tv_usec = 0;
        }        
    }
    
    /* setup the job */
    if (ORTE_SUCCESS != (rc = orte_plm_base_setup_job(jdata))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                         "%s plm:orcm: launching job %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_JOBID_PRINT(jdata->jobid)));
    
    /* set the active jobid */
    active_job = jdata->jobid;

    /* Get the map for this job */
    if (NULL == (map = orte_rmaps.get_job_map(active_job))) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        rc = ORTE_ERR_NOT_FOUND;
        goto cleanup;
    }
    
    if (0 == map->num_new_daemons) {
        /* have all the daemons we need - launch app */
        OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                             "%s plm:orcm: no new daemons to launch",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        failed_launch = false;
        goto cleanup;
    }
    
    if ((0 < opal_output_get_verbosity(orte_plm_globals.output) ||
         orte_leave_session_attached) &&
        mca_plm_orcm_component.num_concurrent < map->num_new_daemons) {
        /**
        * If we are in '--debug-daemons' we keep the ssh connection 
         * alive for the span of the run. If we use this option 
         * AND we launch on more than "num_concurrent" machines
         * then we will deadlock. No connections are terminated 
         * until the job is complete, no job is started
         * since all the orteds are waiting for all the others
         * to come online, and the others ore not launched because
         * we are waiting on those that have started to terminate
         * their ssh tunnels. :(
         * As we cannot run in this situation, pretty print the error
         * and return an error code.
         */
        orte_show_help("help-plm-orcm.txt", "deadlock-params",
                       true, mca_plm_orcm_component.num_concurrent, map->num_new_daemons);
        rc = ORTE_ERR_FATAL;
        goto cleanup;
    }
    
    /*
     * After a discussion between Ralph & Jeff, we concluded that we
     * really are handling the prefix dir option incorrectly. It currently
     * is associated with an app_context, yet it really refers to the
     * location where OpenRTE/Open MPI is installed on a NODE. Fixing
     * this right now would involve significant change to orterun as well
     * as elsewhere, so we will intentionally leave this incorrect at this
     * point. The error, however, is identical to that seen in all prior
     * releases of OpenRTE/Open MPI, so our behavior is no worse than before.
     *
     * A note to fix this, along with ideas on how to do so, has been filed
     * on the project's Trac system under "feature enhancement".
     *
     * For now, default to the prefix_dir provided in the first app_context.
     * Since there always MUST be at least one app_context, we are safe in
     * doing this.
     */
    app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, 0);
    /* we also need at least one node name so we can check what shell is
     * being used, if we have to
     */
    node = NULL;
    for (nnode = 0; nnode < map->nodes->size; nnode++) {
        if (NULL != (node = (orte_node_t*)opal_pointer_array_get_item(map->nodes, nnode))) {
            break;
        }
    }
    if (NULL == node) {
        /* well, if there isn't even one node in the map, then we are hammered */
        rc = ORTE_ERR_FATAL;
        goto cleanup;
    }
    prefix_dir = app->prefix_dir;
    
    /* setup the launch */
    if (ORTE_SUCCESS != (rc = setup_launch(&argc, &argv, node->name, &node_name_index1,
                                           &proc_vpid_index, prefix_dir))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* set the job state to indicate we attempted to launch */
    job_state = ORTE_JOB_STATE_FAILED_TO_START;
    
    /*
     * Iterate through each of the nodes
     */
    for (nnode=0; nnode < map->nodes->size; nnode++) {
        pid_t pid;
        opal_list_item_t *item;
        
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(map->nodes, nnode))) {
            continue;
        }

        /* if this daemon already exists, don't launch it! */
        if (node->daemon_launched) {
            OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                                 "%s plm:orcm:launch daemon already exists on node %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 node->name));
            continue;
        }
        
        /* if the node's daemon has not been defined, then we
         * have an error!
         */
        if (NULL == node->daemon) {
            ORTE_ERROR_LOG(ORTE_ERR_FATAL);
            OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                                 "%s plm:orcm:launch daemon failed to be defined on node %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 node->name));
            rc = ORTE_ERR_FATAL;
            goto cleanup;
        }
        
        /* setup node name */
        free(argv[node_name_index1]);
        if (NULL != node->username &&
            0 != strlen (node->username)) {
            asprintf (&argv[node_name_index1], "%s@%s",
                      node->username, node->name);
        } else {
            argv[node_name_index1] = strdup(node->name);
        }

        OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                             "%s plm:orcm: launching on node %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             node->name));

        /* fork a child to exec the rsh/ssh session */
        pid = fork();
        if (pid < 0) {
            ORTE_ERROR_LOG(ORTE_ERR_SYS_LIMITS_CHILDREN);
            rc = ORTE_ERR_SYS_LIMITS_CHILDREN;
            goto cleanup;
        }

        /* child */
        if (pid == 0) {
            
            /* do the ssh launch - this will exit if it fails */
            ssh_child(argc, argv, node->daemon->name.vpid, proc_vpid_index, node_name_index1);
            
            
        } else { /* father */
            /* indicate this daemon has been launched */
            node->daemon->state = ORTE_PROC_STATE_LAUNCHED;
            /* record the pid */
            node->daemon->pid = pid;
            
            OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                                 "%s plm:orcm: recording launch of daemon %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                ORTE_NAME_PRINT(&node->daemon->name)));

            /* setup callback on sigchild - wait until setup above is complete
             * as the callback can occur in the call to orte_wait_cb
             */
            orte_wait_cb(pid, wait_daemon, (void*)node->daemon);

            OPAL_THREAD_LOCK(&mca_plm_orcm_component.lock);
            /* This situation can lead to a deadlock if '--debug-daemons' is set.
             * However, the deadlock condition is tested at the begining of this
             * function, so we're quite confident it should not happens here.
             */
            if (mca_plm_orcm_component.num_children++ >=
                mca_plm_orcm_component.num_concurrent) {
                opal_condition_wait(&mca_plm_orcm_component.cond, &mca_plm_orcm_component.lock);
            }
            OPAL_THREAD_UNLOCK(&mca_plm_orcm_component.lock);
            
            /* if required - add delay to avoid problems w/ X11 authentication */
            if (0 < opal_output_get_verbosity(orte_plm_globals.output)
                && mca_plm_orcm_component.delay) {
                sleep(mca_plm_orcm_component.delay);
            }
        }
    }

    /* wait for the daemons to report back in the announce callback */
    OPAL_ACQUIRE_THREAD(&orte_plm_globals.spawn_lock,
                        &orte_plm_globals.spawn_in_progress_cond,
                        &orte_plm_globals.spawn_in_progress);
    OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                         "completed spawn for job %s", ORTE_JOBID_PRINT(jdata->jobid)));
    
    /* get here if launch went okay */
    failed_launch = false;
    
    if (orte_timing ) {
        if (0 != gettimeofday(&joblaunchstop, NULL)) {
            opal_output(0, "plm_orcm: could not obtain job launch stop time");
        } else {
            opal_output(0, "plm_orcm: total job launch time is %ld usec",
                        (joblaunchstop.tv_sec - joblaunchstart.tv_sec)*1000000 + 
                        (joblaunchstop.tv_usec - joblaunchstart.tv_usec));
        }
    }

 cleanup:
    if (NULL != argv) {
        opal_argv_free(argv);
    }

    /* RELEASE THE THREAD */
    OPAL_THREAD_UNLOCK(&orte_plm_globals.spawn_lock);

    return rc;
}


/**
 * Terminate the virtual machine
 */
static int terminate_orteds(void)
{
    int rc;
    opal_buffer_t buf;
    orte_daemon_cmd_flag_t cmd = ORTE_DAEMON_HALT_VM_CMD;
    
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    opal_dss.pack(&buf, &cmd, 1, ORTE_DAEMON_CMD_T);
    
    if (ORTE_SUCCESS != (rc = orcm_pnp.output_buffer(ORCM_PNP_SYS_CHANNEL, NULL,
                                                     ORCM_PNP_TAG_COMMAND, &buf))) {
        ORTE_ERROR_LOG(rc);
    }
    
    return rc;
}

static int signal_job(orte_jobid_t jobid, int32_t signal)
{
    int rc;
    opal_buffer_t buf;
    orte_daemon_cmd_flag_t cmd = ORTE_DAEMON_SIGNAL_LOCAL_PROCS;
    
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    opal_dss.pack(&buf, &cmd, 1, ORTE_DAEMON_CMD_T);
    opal_dss.pack(&buf, &jobid, 1, ORTE_JOBID);
    opal_dss.pack(&buf, &signal, 1, OPAL_INT32);
    
    if (ORTE_SUCCESS != (rc = orcm_pnp.output_buffer(ORCM_PNP_SYS_CHANNEL, NULL,
                                                     ORCM_PNP_TAG_COMMAND, &buf))) {
        ORTE_ERROR_LOG(rc);
    }
    
    return rc;
}

static int finalize(void)
{
    return ORTE_SUCCESS;
}


static void set_handler_default(int sig)
{
    struct sigaction act;

    act.sa_handler = SIG_DFL;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);

    sigaction(sig, &act, (struct sigaction *)0);
}

static rsh_shell_t find_shell(char *shell) 
{
    int i         = 0;
    char *sh_name = NULL;
    
    if( (NULL == shell) || (strlen(shell) == 1) ) {
        /* Malformed shell */
        return ORTE_PLM_RSH_SHELL_UNKNOWN;
    }
    
    sh_name = rindex(shell, '/');
    if( NULL == sh_name ) {
        /* Malformed shell */
        return ORTE_PLM_RSH_SHELL_UNKNOWN;
    }
    
    /* skip the '/' */
    ++sh_name;
    for (i = 0; i < (int)(sizeof (rsh_shell_name) /
                          sizeof(rsh_shell_name[0])); ++i) {
        if (0 == strcmp(sh_name, rsh_shell_name[i])) {
            return (rsh_shell_t)i;
        }
    }
    
    /* We didn't find it */
    return ORTE_PLM_RSH_SHELL_UNKNOWN;
}

static int setup_shell(rsh_shell_t *rshell,
                       rsh_shell_t *lshell,
                       char *nodename, int *argc, char ***argv)
{
    rsh_shell_t remote_shell, local_shell;
    struct passwd *p;
    char *param;
    int rc;
    
    /* What is our local shell? */
    local_shell = ORTE_PLM_RSH_SHELL_UNKNOWN;
    p = getpwuid(getuid());
    if( NULL == p ) {
        /* This user is unknown to the system. Therefore, there is no reason we
         * spawn whatsoever in his name. Give up with a HUGE error message.
         */
        orte_show_help( "help-plm-orcm.txt", "unknown-user", true, (int)getuid() );
        return ORTE_ERR_FATAL;
    }
    param = p->pw_shell;
    local_shell = find_shell(p->pw_shell);
    
    /* If we didn't find it in getpwuid(), try looking at the $SHELL
     environment variable (see https://svn.open-mpi.org/trac/ompi/ticket/1060)
     */
    if (ORTE_PLM_RSH_SHELL_UNKNOWN == local_shell && 
        NULL != (param = getenv("SHELL"))) {
        local_shell = find_shell(param);
    }
    
    if (ORTE_PLM_RSH_SHELL_UNKNOWN == local_shell) {
        opal_output(0, "WARNING: local probe returned unhandled shell:%s assuming bash\n",
                    (NULL != param) ? param : "unknown");
        local_shell = ORTE_PLM_RSH_SHELL_BASH;
    }
    
    OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                         "%s plm:orcm: local shell: %d (%s)",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         local_shell, rsh_shell_name[local_shell]));
    
    /* What is our remote shell? */
    if (orte_assume_same_shell) {
        remote_shell = local_shell;
        OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                             "%s plm:orcm: assuming same remote shell as local shell",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    } else {
        rc = orte_plm_rsh_probe(nodename, &remote_shell);
        
        if (ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            return rc;
        }
        
        if (ORTE_PLM_RSH_SHELL_UNKNOWN == remote_shell) {
            opal_output(0, "WARNING: rsh probe returned unhandled shell; assuming bash\n");
            remote_shell = ORTE_PLM_RSH_SHELL_BASH;
        }
    }
    
    OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                         "%s plm:orcm: remote shell: %d (%s)",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         remote_shell, rsh_shell_name[remote_shell]));
    
    /* Do we need to source .profile on the remote side?
     - sh: yes (see bash(1))
     - ksh: yes (see ksh(1))
     - bash: no (see bash(1))
     - [t]csh: no (see csh(1) and tcsh(1))
     - zsh: no (see http://zsh.sourceforge.net/FAQ/zshfaq03.html#l19)
     */
    
    if (ORTE_PLM_RSH_SHELL_SH == remote_shell ||
        ORTE_PLM_RSH_SHELL_KSH == remote_shell) {
        int i;
        char **tmp;
        tmp = opal_argv_split("( test ! -r ./.profile || . ./.profile;", ' ');
        if (NULL == tmp) {
            return ORTE_ERR_OUT_OF_RESOURCE;
        }
        for (i = 0; NULL != tmp[i]; ++i) {
            opal_argv_append(argc, argv, tmp[i]);
        }
        opal_argv_free(tmp);
    }
    
    /* pass results back */
    *rshell = remote_shell;
    *lshell = local_shell;
    
    return ORTE_SUCCESS;
}

/**
 * Check the Shell variable on the specified node
 */

static int rsh_probe(char *nodename, rsh_shell_t *shell)
{
    char ** argv;
    int argc, rc = ORTE_SUCCESS, i;
    int fd[2];
    pid_t pid;
    char outbuf[4096];
    
    OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                         "%s plm:orcm: going to check SHELL variable on node %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         nodename));
    
    *shell = ORTE_PLM_RSH_SHELL_UNKNOWN;
    if (pipe(fd)) {
        OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                             "%s plm:orcm: pipe failed with errno=%d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             errno));
        return ORTE_ERR_IN_ERRNO;
    }
    if ((pid = fork()) < 0) {
        OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                             "%s plm:orcm: fork failed with errno=%d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             errno));
        return ORTE_ERR_IN_ERRNO;
    }
    else if (pid == 0) {          /* child */
        if (dup2(fd[1], 1) < 0) {
            OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                                 "%s plm:orcm: dup2 failed with errno=%d",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 errno));
            exit(01);
        }
        /* Build argv array */
        argv = opal_argv_copy(orte_plm_globals.rsh_agent_argv);
        argc = opal_argv_count(orte_plm_globals.rsh_agent_argv);
        opal_argv_append(&argc, &argv, nodename);
        opal_argv_append(&argc, &argv, "echo $SHELL");
        
        execvp(argv[0], argv);
        exit(errno);
    }
    if (close(fd[1])) {
        OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                             "%s plm:orcm: close failed with errno=%d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             errno));
        return ORTE_ERR_IN_ERRNO;
    }
    
    {
        ssize_t ret = 1;
        char* ptr = outbuf;
        size_t outbufsize = sizeof(outbuf);
        
        do {
            ret = read (fd[0], ptr, outbufsize-1);
            if (ret < 0) {
                if (errno == EINTR)
                    continue;
                OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                                     "%s plm:orcm: Unable to detect the remote shell (error %s)",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     strerror(errno)));
                rc = ORTE_ERR_IN_ERRNO;
                break;
            }
            if( outbufsize > 1 ) {
                outbufsize -= ret;
                ptr += ret;
            }
        } while( 0 != ret );
        *ptr = '\0';
    }
    close(fd[0]);
    
    if( outbuf[0] != '\0' ) {
        char *sh_name = rindex(outbuf, '/');
        if( NULL != sh_name ) {
            sh_name++; /* skip '/' */
            /* We cannot use "echo -n $SHELL" because -n is not portable. Therefore
             * we have to remove the "\n" */
            if ( sh_name[strlen(sh_name)-1] == '\n' ) {
                sh_name[strlen(sh_name)-1] = '\0';
            }
            /* Search for the substring of known shell-names */
            for (i = 0; i < (int)(sizeof (rsh_shell_name)/
                                  sizeof(rsh_shell_name[0])); i++) {
                if ( 0 == strcmp(sh_name, rsh_shell_name[i]) ) {
                    *shell = (rsh_shell_t)i;
                    break;
                }
            }
        }
    }
    
    OPAL_OUTPUT_VERBOSE((1, orte_plm_globals.output,
                         "%s plm:orcm: node %s has SHELL: %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         nodename,
                         (ORTE_PLM_RSH_SHELL_UNKNOWN == *shell) ? "UNHANDLED" : (char*)rsh_shell_name[*shell]));
    
    return rc;
}

static void daemon_callback(int status,
                            orte_process_name_t *sender,
                            orcm_pnp_tag_t tag,
                            opal_buffer_t *buf,
                            void *cbdata)
{
    opal_output(0, "%s RECEIVED DAEMON CALLBACK FROM %s",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_NAME_PRINT(sender));
}
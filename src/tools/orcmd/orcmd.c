/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "openrcm_config_private.h"

/* add the openrcm definitions */
#include "include/constants.h"
#include "runtime/runtime.h"

#include "orte_config.h"
#include "orte/constants.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <stdio.h>
#include <ctype.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif  /* HAVE_SYS_TIME_H */


#include "opal/event/event.h"
#include "opal/mca/base/base.h"
#include "opal/util/output.h"
#include "opal/util/cmd_line.h"
#include "opal/util/opal_environ.h"
#include "opal/util/os_path.h"
#include "opal/util/printf.h"
#include "opal/util/argv.h"
#include "opal/runtime/opal.h"
#include "opal/mca/base/mca_base_param.h"
#include "opal/util/daemon_init.h"
#include "opal/dss/dss.h"

#include "orte/constants.h"
#include "orte/util/show_help.h"
#include "orte/util/proc_info.h"
#include "orte/util/session_dir.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_locks.h"
#include "orte/mca/rml/base/rml_contact.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/grpcomm/grpcomm.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/rml/rml_types.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/plm/plm.h"
#include "orte/mca/ras/ras.h"
#include "orte/mca/routed/routed.h"

#include "mca/pnp/pnp.h"
#include "mca/pnp/base/public.h"

/*
 * Globals
 */

char *log_path = NULL;
static opal_event_t *orcmd_exit_event;
static bool signals_set=false;
static bool orcmd_spin_flag=false;

static void shutdown_callback(int fd, short flags, void *arg);
static void shutdown_signal(int fd, short flags, void *arg);

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       opal_buffer_t *buf,
                       void *cbdata);
static struct {
    bool debug;
    bool help;
    bool set_sid;
    bool daemonize;
    char* name;
    char* vpid_start;
    char* num_procs;
    int fail;
    int fail_delay;
    bool abort;
    int heartbeat;
} orcmd_globals;

/*
 * define the orcmd context table for obtaining parameters
 */
opal_cmd_line_init_t orte_cmd_line_opts[] = {
    /* Various "obvious" options */
    { NULL, NULL, NULL, 'h', NULL, "help", 0,
      &orcmd_globals.help, OPAL_CMD_LINE_TYPE_BOOL,
      "This help message" },

    { "orte", "daemon_spin", NULL, 's', NULL, "spin", 0,
      &orcmd_spin_flag, OPAL_CMD_LINE_TYPE_BOOL,
      "Have the orcmd spin until we can connect a debugger to it" },

    { NULL, NULL, NULL, '\0', NULL, "heartbeat", 1,
      &orcmd_globals.heartbeat, OPAL_CMD_LINE_TYPE_INT,
      "Seconds between orcmd heartbeat messages to be sent to HNP (default: 0 => no heartbeat)" },
    
    { "orte", "debug", NULL, 'd', NULL, "debug", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Debug the OpenRTE" },
        
    { "orte", "daemonize", NULL, '\0', NULL, "daemonize", 0,
      &orcmd_globals.daemonize, OPAL_CMD_LINE_TYPE_BOOL,
      "Daemonize the orcmd into the background" },

    { "orte", "debug", "daemons", '\0', NULL, "debug-daemons", 0,
      &orcmd_globals.debug, OPAL_CMD_LINE_TYPE_BOOL,
      "Enable debugging of OpenRTE daemons" },

    { "orte", "debug", "daemons_file", '\0', NULL, "debug-daemons-file", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Enable debugging of OpenRTE daemons, storing output in files" },

    { "orte", "parent", "uri", '\0', NULL, "parent-uri", 1,
      NULL, OPAL_CMD_LINE_TYPE_STRING,
      "URI for the parent if tree launch is enabled."},
    
    { NULL, NULL, NULL, '\0', NULL, "set-sid", 0,
      &orcmd_globals.set_sid, OPAL_CMD_LINE_TYPE_BOOL,
      "Direct the orcmd to separate from the current session"},
    
    { "tmpdir", "base", NULL, '\0', NULL, "tmpdir", 1,
      NULL, OPAL_CMD_LINE_TYPE_STRING,
      "Set the root for the session directory tree" },

    { "orte", "output", "filename", '\0', "output-filename", "output-filename", 1,
      NULL, OPAL_CMD_LINE_TYPE_STRING,
      "Redirect output from application processes into filename.rank" },
    
    { "orte", "xterm", NULL, '\0', "xterm", "xterm", 1,
      NULL, OPAL_CMD_LINE_TYPE_STRING,
      "Create a new xterm window and display output from the specified ranks there" },

    { "orte", "daemon", "bootstrap", '\0', "bootstrap", "bootstrap", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Bootstrap the connection to the HNP" },
    
    /* End of list */
    { NULL, NULL, NULL, '\0', NULL, NULL, 0,
      NULL, OPAL_CMD_LINE_TYPE_NULL, NULL }
};

int main(int argc, char *argv[])
{
    int ret = 0;
    int fd;
    opal_cmd_line_t *cmd_line = NULL;
    char log_file[PATH_MAX];
    char *jobidstring;
    char *rml_uri;
    int i;
    opal_buffer_t *buffer;
    char hostname[100];
    char *tmp_env_var = NULL;
    struct timeval starttime, setuptime;
    
    /* get our time for first executable */
    gettimeofday(&starttime, NULL);
    
    /* initialize the globals */
    memset(&orcmd_globals, 0, sizeof(orcmd_globals));
    
    /* setup to check common command line options that just report and die */
    cmd_line = OBJ_NEW(opal_cmd_line_t);
    opal_cmd_line_create(cmd_line, orte_cmd_line_opts);
    mca_base_cmd_line_setup(cmd_line);
    if (ORTE_SUCCESS != (ret = opal_cmd_line_parse(cmd_line, false,
                                                   argc, argv))) {
        char *args = NULL;
        args = opal_cmd_line_get_usage_msg(cmd_line);
        orte_show_help("help-orcmd.txt", "orcmd:usage", false,
                       argv[0], args);
        free(args);
        return ret;
    }
    
    /* check for help request */
    if (orcmd_globals.help) {
        char *args = NULL;
        args = opal_cmd_line_get_usage_msg(cmd_line);
        orte_show_help("help-orcmd.txt", "orcmd:usage", false,
                       argv[0], args);
        free(args);
        return 1;
    }

    /*
     * Since this process can now handle MCA/GMCA parameters, make sure to
     * process them.
     */
    mca_base_cmd_line_process_args(cmd_line, &environ, &environ);
    
    /* Ensure that enough of OPAL is setup for us to be able to run */
    /*
     * NOTE: (JJH)
     *  We need to allow 'mca_base_cmd_line_process_args()' to process command
     *  line arguments *before* calling opal_init_util() since the command
     *  line could contain MCA parameters that affect the way opal_init_util()
     *  functions. AMCA parameters are one such option normally received on the
     *  command line that affect the way opal_init_util() behaves.
     *  It is "safe" to call mca_base_cmd_line_process_args() before 
     *  opal_init_util() since mca_base_cmd_line_process_args() does *not*
     *  depend upon opal_init_util() functionality.
     */
    if (OPAL_SUCCESS != opal_init_util(&argc, &argv)) {
        fprintf(stderr, "OPAL failed to initialize -- orcmd aborting\n");
        exit(1);
    }

    /* setup the exit triggers */
    OBJ_CONSTRUCT(&orte_exit, orte_trigger_event_t);
 
    /* save the environment for launch purposes. This MUST be
     * done so that we can pass it to any local procs we
     * spawn - otherwise, those local procs won't see any
     * non-MCA envars that were set in the enviro when the
     * orcmd was executed - e.g., by .csh
     */
    orte_launch_environ = opal_argv_copy(environ);
    
    
    /* if orte_daemon_debug is set, let someone know we are alive right
     * away just in case we have a problem along the way
     */
    if (orcmd_globals.debug) {
        gethostname(hostname, 100);
        fprintf(stderr, "Daemon was launched on %s - beginning to initialize\n", hostname);
    }
    
#if defined(HAVE_SETSID) && !defined(__WINDOWS__)
    /* see if we were directed to separate from current session */
    if (orcmd_globals.set_sid) {
        setsid();
    }
#endif  /* !defined(__WINDOWS__) */
    /* see if they want us to spin until they can connect a debugger to us */
    i=0;
    while (orcmd_spin_flag) {
        i++;
        if (1000 < i) i=0;        
    }

    /* init the ORCM library - this includes registering
     * a multicast recv so we hear announcements and
     * their responses from other apps
     */
    if (ORCM_SUCCESS != (ret = orcm_init(OPENRCM_DAEMON))) {
        fprintf(stderr, "Failed to init: error %d\n", ret);
        exit(1);
    }
    
    /* output a message indicating we are alive, our name, and our pid
     * for debugging purposes
     */
    if (orte_debug_daemons_flag) {
        fprintf(stderr, "Daemon %s checking in as pid %ld on host %s\n",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (long)orte_process_info.pid,
                orte_process_info.nodename);
    }
    
    /* detach from controlling terminal
     * otherwise, remain attached so output can get to us
     */
    if(!orte_debug_flag && !orte_debug_daemons_flag && orcmd_globals.daemonize) {
        opal_daemon_init(NULL);
    }
    
#if 0
    /* insert our contact info into our process_info struct so we
     * have it for later use and set the local daemon field to our name
     */
    orte_process_info.my_daemon_uri = orte_rml.get_contact_info();
    ORTE_PROC_MY_DAEMON->jobid = ORTE_PROC_MY_NAME->jobid;
    ORTE_PROC_MY_DAEMON->vpid = ORTE_PROC_MY_NAME->vpid;
#endif
    
    /* setup stdout/stderr */
    if (orte_debug_daemons_file_flag) {
        /* if we are debugging to a file, then send stdout/stderr to
         * the orcmd log file
         */
        
        /* get my jobid */
        if (ORTE_SUCCESS != (ret = orte_util_convert_jobid_to_string(&jobidstring,
                                                                     ORTE_PROC_MY_NAME->jobid))) {
            ORTE_ERROR_LOG(ret);
            goto DONE;
        }
        
        /* define a log file name in the session directory */
        snprintf(log_file, PATH_MAX, "output-orcmd-%s-%s.log",
                 jobidstring, orte_process_info.nodename);
        log_path = opal_os_path(false,
                                orte_process_info.tmpdir_base,
                                orte_process_info.top_session_dir,
                                log_file,
                                NULL);
        
        fd = open(log_path, O_RDWR|O_CREAT|O_TRUNC, 0640);
        if (fd < 0) {
            /* couldn't open the file for some reason, so
             * just connect everything to /dev/null
             */
            fd = open("/dev/null", O_RDWR|O_CREAT|O_TRUNC, 0666);
        } else {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if(fd != STDOUT_FILENO && fd != STDERR_FILENO) {
                close(fd);
            }
        }
    }
    
    /* setup an event we can wait for to tell
     * us to terminate - both normal and abnormal
     * termination will call us here. Use the same exit
     * fd as orterun so that orte_comm can wake either of us up
     * since we share that code
     */
    if (ORTE_SUCCESS != (ret = orte_wait_event(&orcmd_exit_event, &orte_exit, "orcmd_shutdown", shutdown_callback))) {
        ORTE_ERROR_LOG(ret);
        goto DONE;
    }
    
    /* We actually do *not* want the orcmd to voluntarily yield() the
     processor more than necessary.  The orcmd already blocks when
     it is doing nothing, so it doesn't use any more CPU cycles than
     it should; but when it *is* doing something, we do not want it
     to be unnecessarily delayed because it voluntarily yielded the
     processor in the middle of its work.
     
     For example: when a message arrives at the orcmd, we want the
     OS to wake up the orcmd in a timely fashion (which most OS's
     seem good about doing) and then we want the orcmd to process
     the message as fast as possible.  If the orcmd yields and lets
     aggressive MPI applications get the processor back, it may be a
     long time before the OS schedules the orcmd to run again
     (particularly if there is no IO event to wake it up).  Hence,
     routed OOB messages (for example) may be significantly delayed
     before being delivered to MPI processes, which can be
     problematic in some scenarios (e.g., COMM_SPAWN, BTL's that
     require OOB messages for wireup, etc.). */
    opal_progress_set_yield_when_idle(false);
    
    /* Change the default behavior of libevent such that we want to
     continually block rather than blocking for the default timeout
     and then looping around the progress engine again.  There
     should be nothing in the orcmd that cannot block in libevent
     until "something" happens (i.e., there's no need to keep
     cycling through progress because the only things that should
     happen will happen in libevent).  This is a minor optimization,
     but what the heck... :-) */
    opal_progress_set_event_flag(OPAL_EVLOOP_ONCE);
    
    /* setup the heartbeat - the heartbeat will carry any messages
     * from us
     */
    if (0 < orcmd_globals.heartbeat) {
        orte_heartbeat_rate = orcmd_globals.heartbeat;
        orcm_pnp_base_start_heart("ORCMD", "0.1", "alpha");
    }
    
    /* register an input to hear our peers */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_input_buffer("ORCMD", "0.1", "alpha",
                                                              ORCM_PNP_GROUP_OUTPUT_CHANNEL,
                                                              ORCM_PNP_TAG_WILDCARD, recv_input))) {
        ORTE_ERROR_LOG(ret);
        goto DONE;
    }
    /* announce our existence - this carries with it our rml uri and
     * our local node system info
     */
    if (ORCM_SUCCESS != (ret = orcm_pnp.announce("ORCMD", "0.1", "alpha", NULL))) {
        ORTE_ERROR_LOG(ret);
        goto DONE;
    }
    
   if (orte_debug_daemons_flag) {
        opal_output(0, "%s orcmd: up and running - waiting for commands!", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    }

    /* wait to hear we are done */
    opal_event_dispatch();

    /* should never get here, but if we do... */
DONE:
    /* cleanup any lingering session directories */
    orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
    
    /* cleanup the triggers */
    OBJ_DESTRUCT(&orte_exit);

    /* Finalize and clean up ourselves */
    orcm_finalize();
    return ret;
}

static void shutdown_callback(int fd, short flags, void *arg)
{
    int ret;
    
    if (orte_debug_daemons_flag) {
        opal_output(0, "%s orcmd: finalizing", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    }
    
    /* cleanup */
    if (NULL != log_path) {
        unlink(log_path);
    }
    
    /* make sure our local procs are dead */
    orte_odls.kill_local_procs(NULL);
    
    /* whack any lingering session directory files from our jobs */
    orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
    
    /* cleanup the triggers */
    OBJ_DESTRUCT(&orte_exit);

    /* if we were ordered to abort, do so */
    if (orcmd_globals.abort) {
        opal_output(0, "%s is executing clean abort", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        /* do -not- call finalize as this will send a message to the HNP
         * indicating clean termination! Instead, just forcibly cleanup
         * the local session_dir tree and abort
         */
        orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
        abort();
    } else if ((int)ORTE_PROC_MY_NAME->vpid == orcmd_globals.fail) {
        opal_output(0, "%s is executing clean abnormal termination", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        /* do -not- call finalize as this will send a message to the HNP
         * indicating clean termination! Instead, just forcibly cleanup
         * the local session_dir tree and exit
         */
        orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
        exit(ORTE_ERROR_DEFAULT_EXIT_CODE);
    }

    /* Finalize and clean up ourselves */
    orcm_finalize();
    exit(orte_exit_status);
}

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       opal_buffer_t *buf,
                       void *cbdata)
{
    
}

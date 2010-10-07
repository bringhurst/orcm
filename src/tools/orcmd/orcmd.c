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
#include <stddef.h>
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
#include "opal/mca/sysinfo/sysinfo.h"

#include "orte/constants.h"
#include "orte/util/show_help.h"
#include "orte/util/proc_info.h"
#include "orte/util/session_dir.h"
#include "orte/util/name_fns.h"
#include "orte/util/nidmap.h"
#include "orte/runtime/orte_locks.h"
#include "orte/runtime/orte_quit.h"
#include "orte/mca/rml/base/rml_contact.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/odls/base/base.h"
#include "orte/mca/plm/plm.h"
#include "orte/mca/plm/base/plm_private.h"
#include "orte/mca/ras/ras.h"
#include "orte/mca/routed/routed.h"

#include "mca/pnp/pnp.h"
#include "mca/pnp/base/public.h"
#include "runtime/runtime.h"
/*
 * Globals
 */
static bool orcmd_spin_flag=false;

static int orcmd_comm(orte_process_name_t *recipient,
                      opal_buffer_t *buf, orte_rml_tag_t tag,
                      orte_default_cbfunc_t cbfunc);

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
    char *hnp_uri;
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
    
    { "orte", "hnp", "uri", '\0', "hnp-uri", "hnp-uri", 1,
      NULL, OPAL_CMD_LINE_TYPE_STRING,
      "URI for the HNP that booted this VM" },
    
    /* End of list */
    { NULL, NULL, NULL, '\0', NULL, NULL, 0,
      NULL, OPAL_CMD_LINE_TYPE_NULL, NULL }
};

int main(int argc, char *argv[])
{
    int ret = 0;
    opal_cmd_line_t *cmd_line = NULL;
    int i;
    opal_buffer_t *buffer;
    char hostname[100];
    char *tmp_env_var = NULL;
    
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

    /* save the environment for launch purposes. This MUST be
     * done so that we can pass it to any local procs we
     * spawn - otherwise, those local procs won't see any
     * non-MCA envars that were set in the enviro when the
     * orcmd was executed - e.g., by .csh
     */
    orte_launch_environ = opal_argv_copy(environ);
    
    /* if orte_daemon_debug is set, let someone know we are alive right
     * away just in case we have a problem along the way - we won't have
     * processed the orte mca params yet, so use the local flag
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
    if (ORCM_SUCCESS != (ret = orcm_init(ORCM_DAEMON))) {
        fprintf(stderr, "Failed to init: error %d\n", ret);
        exit(1);
    }
    
#if 0    
    /* detach from controlling terminal
     * otherwise, remain attached so output can get to us
     */
    if(!orte_debug_flag && !orte_debug_daemons_flag && orcmd_globals.daemonize) {
        opal_daemon_init(NULL);
    } else {
        /* set the local debug verbosity */
        orcm_debug_output = 5;
    }
#endif

    /* set the odls comm function */
    orte_comm = orcmd_comm;

    /* wait to hear we are done */
    opal_event_dispatch();
    return ret;

    /* should never get here, but if we do... */
DONE:
    /* cleanup any lingering session directories */
    orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
    
    /* Finalize and clean up ourselves */
    orcm_finalize();
    return ret;
}

static int orcmd_comm(orte_process_name_t *recipient,
                      opal_buffer_t *buf, orte_rml_tag_t tag,
                      orte_default_cbfunc_t cbfunc)
{
    opal_output(0, "%s comm to %s tag %d", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_NAME_PRINT(recipient), tag);
    return ORCM_SUCCESS;
}

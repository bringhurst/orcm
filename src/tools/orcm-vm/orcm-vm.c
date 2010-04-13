/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved. 
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

#include <stdio.h>
#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif  /*  HAVE_STDLIB_H */
#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif  /*  HAVE_SIGNAL_H */
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif  /* HAVE_SYS_STAT_H */
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif  /* HAVE_SYS_TYPES_H */
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif  /* HAVE_SYS_WAIT_H */
#ifdef HAVE_STRING_H
#include <string.h>
#endif  /* HAVE_STRING_H */
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif  /* HAVE_DIRENT_H */
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif  /* HAVE_PWD_H */

#include "opal/dss/dss.h"
#include "opal/class/opal_value_array.h"
#include "opal/event/event.h"
#include "opal/util/cmd_line.h"
#include "opal/util/argv.h"
#include "opal/util/opal_environ.h"
#include "opal/util/path.h"
#include "opal/mca/base/base.h"
#include "opal/mca/base/mca_base_param.h"
#include "opal/runtime/opal.h"
#include "opal/mca/installdirs/installdirs.h"
#include "opal/util/basename.h"
#include "opal/mca/sysinfo/sysinfo.h"
#include "opal/threads/threads.h"

#include "orte/runtime/runtime.h"
#include "orte/util/show_help.h"
#include "orte/util/parse_options.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"
#include "orte/runtime/orte_locks.h"
#include "orte/mca/plm/plm.h"
#include "orte/mca/plm/base/plm_private.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/rmcast/base/base.h"

#include "mca/pnp/pnp.h"

/*****************************************
 * Global Vars for Command line Arguments
 *****************************************/
static struct {
    bool help;
    bool boot;
    int halt;
    int verbosity;
    char *hostfile;
    char *hosts;
    char *report_master;
} my_globals;

opal_cmd_line_init_t cmd_line_opts[] = {
    { NULL, NULL, NULL, 'h', NULL, "help", 0,
      &my_globals.help, OPAL_CMD_LINE_TYPE_BOOL,
      "Print help message" },

    { NULL, NULL, NULL, 'd', "debug", "debug", 1,
      &my_globals.verbosity, OPAL_CMD_LINE_TYPE_INT,
      "Debug verbosity (default: 0)" },
    
    { NULL, NULL, NULL, 'b', "boot", "boot", 0,
      &my_globals.boot, OPAL_CMD_LINE_TYPE_BOOL,
      "Start a virtual machine (default)" },

    { NULL, NULL, NULL, 'k', "halt", "halt", 1,
      &my_globals.halt, OPAL_CMD_LINE_TYPE_INT,
      "Stop the specified distributed virtual machine" },

    { NULL, NULL, NULL, 'h', "hostfile", "hostfile", 1,
      &my_globals.hostfile, OPAL_CMD_LINE_TYPE_STRING,
      "Name of file containing hosts upon which daemons are to be started" },
    
    { NULL, NULL, NULL, 'H', "host", "host", 1,
      &my_globals.hosts, OPAL_CMD_LINE_TYPE_STRING,
      "Comma-delimited list of hosts upon which daemons are to be started" },
    
    { NULL, NULL, NULL, '\0', "report-master", "report-master", 1,
      &my_globals.report_master, OPAL_CMD_LINE_TYPE_STRING,
      "Printout master ID of the virtual machine on stdout [-], stderr [+], or a file [anything else]" },

    { "orte", "debug", NULL, 'd', "debug-devel", "debug-devel", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Enable debugging of OpenRTE" },
    
    { "orte", "debug", "daemons", '\0', "debug-daemons", "debug-daemons", 0,
      NULL, OPAL_CMD_LINE_TYPE_INT,
      "Enable debugging of any daemons used by this application" },
    
    { "orte", "debug", "daemons_file", '\0', "debug-daemons-file", "debug-daemons-file", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Enable debugging of any daemons used by this application, storing output in files" },
    
    /* End of list */
    { NULL, NULL, NULL, '\0', NULL, NULL, 0,
      NULL, OPAL_CMD_LINE_TYPE_NULL,
      NULL }
};

/*
 * Local variables & functions
 */
static orte_job_t *daemons;

static void vm_tracker(char *app, char *version, char *release,
                       orte_process_name_t *name, char *node);

int main(int argc, char *argv[])
{
    int ret;
    opal_cmd_line_t cmd_line;
    orte_proc_t *proc;
    orte_app_context_t *app=NULL;
    opal_buffer_t buf;
    orte_daemon_cmd_flag_t cmd=ORTE_DAEMON_HALT_VM_CMD;
    int32_t ljob;
    uint16_t jfam;
    
    /***************
     * Initialize
     ***************/
    
    /* initialize the globals */
    my_globals.help = false;
    my_globals.verbosity = 0;
    my_globals.boot = false;
    my_globals.halt = -1;
    my_globals.hostfile = NULL;
    my_globals.hosts = NULL;
    my_globals.report_master = NULL;
    
    /* Parse the command line options */
    opal_cmd_line_create(&cmd_line, cmd_line_opts);
    mca_base_cmd_line_setup(&cmd_line);
    ret = opal_cmd_line_parse(&cmd_line, false, argc, argv);
    
    /* extract the MCA/GMCA params */
    mca_base_cmd_line_process_args(&cmd_line, &environ, &environ);

    /* Ensure that enough of OPAL etc. is setup for us to be able to run */
    if( ORTE_SUCCESS != (ret = orcm_init_util()) ) {
        return ret;
    }

    /**
     * Now start parsing our specific arguments
     */
    if (OPAL_SUCCESS != ret || my_globals.help) {
        char *args = NULL;
        args = opal_cmd_line_get_usage_msg(&cmd_line);
        orte_show_help("help-orcm-vm.txt", "usage", true, args);
        free(args);
        return ORTE_ERROR;
    }
    
    /* check for bozo case */
    if (my_globals.boot && 0 <= my_globals.halt) {
        orte_show_help("help-orcm-vm.txt", "boot-halt", true);
        return ORTE_ERROR;
    }
    
    /* if halt was not given, then default to boot */
    if (0 > my_globals.halt) {
        my_globals.boot = true;
    }

    /* open a debug channel and set the verbosity */
    orcm_debug_output = opal_output_open(NULL);
    opal_output_set_verbosity(orcm_debug_output, my_globals.verbosity);
    
    /* save the environment for launch purposes. This MUST be
     * done so that we can pass it to any local procs we
     * spawn - otherwise, those local procs won't see any
     * non-MCA envars were set in the enviro
     */
    orte_launch_environ = opal_argv_copy(environ);
    
    /* set the launch agent to "orcmd" */
    putenv("OMPI_MCA_orte_launch_agent=orcmd");
    
    /***************************
     * We need all of OPAL and ORTE
     ***************************/
    if (my_globals.boot) {
        /* must register as master so we can launch */
        if (ORTE_SUCCESS != orcm_init(ORCM_MASTER)) {
            orcm_finalize();
            return 1;
        }
        /* lookup the daemon job data object */
        if (NULL == (daemons = orte_get_job_data_object(ORTE_PROC_MY_NAME->jobid))) {
            opal_output(orte_clean_output, "COULD NOT GET DAEMON JOB OBJECT");
            goto cleanup;
        }
        /* ensure the accounting starts correctly */
        daemons->num_reported = 1;
        
    } else {
        /* register as tool */
        if (ORTE_SUCCESS != orcm_init(ORCM_TOOL)) {
            orcm_finalize();
            return 1;
        }
    }

    /* announce our existence - this carries with it our rml uri and
     * our local node system info
     */
    if (ORCM_SUCCESS != (ret = orcm_pnp.announce("ORCM-VM", "0.1", "alpha", vm_tracker))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }

    if (my_globals.boot) {
        /* create an app */
        app = OBJ_NEW(orte_app_context_t);
        app->app = strdup("ORCMD");
        /* add to the daemon job - always must be an app for a job */
        opal_pointer_array_add(daemons->apps, app);
        
        /* if we were given hosts to startup, create an app for the
         * daemon job so it can start the virtual machine
         */
        if (NULL != my_globals.hosts || NULL != my_globals.hostfile) {
            if (NULL != my_globals.hosts) {
                app->dash_host = opal_argv_split(my_globals.hosts, ',');
            }
            if (NULL != my_globals.hostfile) {
                app->hostfile = strdup(my_globals.hostfile);
            }
        }
        
        /* turn off any mapping policies */
        ORTE_SET_MAPPING_POLICY(0);
        
        /* ensure we always utilize the local node as well */
        orte_hnp_is_allocated = true;
        
        /* launch the virtual machine - this will launch a daemon on
         * each node. It will simply return if there are no known
         * nodes other than the one we are on
         */
        if (ORTE_SUCCESS != orte_plm.spawn(daemons)) {
            opal_output(orte_clean_output, "\nORCM DISTRIBUTED VIRTUAL MACHINE %s FAILED TO LAUNCH\n",
                        ORTE_JOB_FAMILY_PRINT(ORTE_PROC_MY_NAME->jobid));
            goto cleanup;
        }
        
        /* check for request to report master */
        if (NULL != my_globals.report_master) {
            FILE *fp;
            if (0 == strcmp(my_globals.report_master, "-")) {
                /* if '-', then output to stdout */
                printf("%s\n", ORTE_JOB_FAMILY_PRINT(ORTE_PROC_MY_NAME->jobid));
            } else if (0 == strcmp(my_globals.report_master, "+")) {
                /* if '+', output to stderr */
                fprintf(stderr, "%s\n", ORTE_JOB_FAMILY_PRINT(ORTE_PROC_MY_NAME->jobid));
            } else {
                fp = fopen(my_globals.report_master, "w");
                if (NULL == fp) {
                    orte_show_help("help-orcm.txt", "orcm-vm:write_file", false,
                                   "orcm-vm", "master id", my_globals.report_master);
                    exit(0);
                }
                fprintf(fp, "%s\n", ORTE_JOB_FAMILY_PRINT(ORTE_PROC_MY_NAME->jobid));
                fclose(fp);
            }
        } else {
            opal_output(orte_clean_output, "\nORCM DISTRIBUTED VIRTUAL MACHINE %s RUNNING...\n",
                        ORTE_JOB_FAMILY_PRINT(ORTE_PROC_MY_NAME->jobid));
        }        
    } else {
        /* we are ordering a halt */
        OBJ_CONSTRUCT(&buf, opal_buffer_t);
        /* pack the target DVM */
        jfam = my_globals.halt & 0x0000ffff;
        opal_dss.pack(&buf, &jfam, 1, OPAL_UINT16);
        /* pack the command */
        opal_dss.pack(&buf, &cmd, 1, ORTE_DAEMON_CMD);
        if (ORCM_SUCCESS != (ret = orcm_pnp.output_buffer(ORCM_PNP_SYS_CHANNEL, NULL,
                                                          ORCM_PNP_TAG_COMMAND, &buf))) {
            ORTE_ERROR_LOG(ret);
        }
        OBJ_DESTRUCT(&buf);
    }

    

    /***************
     * Cleanup
     ***************/
 cleanup:
    ORTE_UPDATE_EXIT_STATUS(orte_exit_status);
    
    /* Remove the signal handlers */
    orcm_remove_signal_handlers();
    
    /* whack any lingering session directory files from our jobs */
    orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
    
    /* cleanup and leave */
    orcm_finalize();
    
    exit(orte_exit_status);
    return orte_exit_status;
}

static void vm_tracker(char *app, char *version, char *release,
                       orte_process_name_t *name, char *node)
{
    /* if this isn't a daemon I launched, ignore it */
    if (name->jobid != ORTE_PROC_MY_NAME->jobid) {
        return;
    }
    
    /* track number we have heard from */
    daemons->num_reported++;
    
    /* check if we have heard from them all */
    if (daemons->num_procs <= daemons->num_reported) {
        OPAL_WAKEUP_THREAD(&orte_plm_globals.spawn_in_progress_cond,
                           &orte_plm_globals.spawn_in_progress);
    }
}

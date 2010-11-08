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
#include "opal/mca/event/event.h"
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
#include "orte/runtime/orte_wait.h"
#include "orte/mca/plm/plm.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/rmcast/base/base.h"
#include "orte/mca/rmaps/rmaps_types.h"
#include "orte/mca/odls/odls_types.h"

#include "mca/pnp/pnp.h"

/*****************************************
 * Global Vars for Command line Arguments
 *****************************************/
static struct {
    bool help;
    int verbosity;
    char *hostfile;
    char *hosts;
    char *report_uri;
} my_globals;

opal_cmd_line_init_t cmd_line_opts[] = {
    { NULL, NULL, NULL, 'h', NULL, "help", 0,
      &my_globals.help, OPAL_CMD_LINE_TYPE_BOOL,
      "Print help message" },

    { NULL, NULL, NULL, 'd', "debug", "debug", 1,
      &my_globals.verbosity, OPAL_CMD_LINE_TYPE_INT,
      "Debug verbosity (default: 0)" },
    
    { NULL, NULL, NULL, 'h', "hostfile", "hostfile", 1,
      &my_globals.hostfile, OPAL_CMD_LINE_TYPE_STRING,
      "Name of file containing hosts upon which daemons are to be started" },
    
    { NULL, NULL, NULL, 'H', "host", "host", 1,
      &my_globals.hosts, OPAL_CMD_LINE_TYPE_STRING,
      "Comma-delimited list of hosts upon which daemons are to be started" },
    
    { NULL, NULL, NULL, '\0', "report-uri", "report-uri", 1,
      &my_globals.report_uri, OPAL_CMD_LINE_TYPE_STRING,
      "Printout URI of the ORCM DVM on stdout [-], stderr [+], or a file [anything else]" },

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

int main(int argc, char *argv[])
{
    int ret, i;
    opal_cmd_line_t cmd_line;
    orte_proc_t *proc;
    orte_app_context_t *app=NULL;
    opal_buffer_t buf;
    orte_daemon_cmd_flag_t cmd=ORTE_DAEMON_HALT_VM_CMD;
    int32_t ljob;
    uint16_t jfam;
    char dir[MAXPATHLEN];
    char *dvm;
    
    /***************
     * Initialize
     ***************/
    
    /* initialize the globals */
    my_globals.help = false;
    my_globals.verbosity = 0;
    my_globals.hostfile = NULL;
    my_globals.hosts = NULL;
    my_globals.report_uri = NULL;

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
        orte_show_help("help-orcm.txt", "usage", true, args);
        free(args);
        return ORTE_ERROR;
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
    /* must register as master so we can launch */
    if (ORTE_SUCCESS != orcm_init(ORCM_MASTER)) {
        orcm_finalize();
        return 1;
    }
    
    /* check for request to report uri */
    if (NULL != my_globals.report_uri) {
        FILE *fp;
        char *rml_uri;
        rml_uri = orte_rml.get_contact_info();
        if (0 == strcmp(my_globals.report_uri, "-")) {
            /* if '-', then output to stdout */
            printf("%s\n",  (NULL == rml_uri) ? "NULL" : rml_uri);
        } else if (0 == strcmp(my_globals.report_uri, "+")) {
            /* if '+', output to stderr */
            fprintf(stderr, "%s\n",  (NULL == rml_uri) ? "NULL" : rml_uri);
        } else {
            fp = fopen(my_globals.report_uri, "w");
            if (NULL == fp) {
                orte_show_help("help-orcm.txt", "orcm:write_file", false,
                               "orcm", "uri", my_globals.report_uri);
                orcm_finalize();
                exit(1);
            }
            fprintf(fp, "%s\n", (NULL == rml_uri) ? "NULL" : rml_uri);
            fclose(fp);
        }
        if (NULL != rml_uri) {
            free(rml_uri);
        }        
    }
    
    /* lookup the daemon job data object */
    if (NULL == (daemons = orte_get_job_data_object(ORTE_PROC_MY_NAME->jobid))) {
        opal_output(orte_clean_output, "COULD NOT GET DAEMON JOB OBJECT");
        goto cleanup;
    }
    /* ensure the accounting starts correctly */
    daemons->num_reported = 1;
    app = (orte_app_context_t*)opal_pointer_array_get_item(daemons->apps, 0);

    /* if we were given hosts to startup, add them to the global
     * node array
     */
    if (NULL != my_globals.hosts || NULL != my_globals.hostfile) {
        if (NULL != my_globals.hosts) {
            app->dash_host = opal_argv_split(my_globals.hosts, ',');
        }
        if (NULL != my_globals.hostfile) {
            app->hostfile = strdup(my_globals.hostfile);
        }
    }
    
    /* ensure we always utilize the local node as well */
    orte_hnp_is_allocated = true;
    
    /* set the number of daemons to launch to be the number
     * of nodes known to the system
     */
    app->num_procs = 0;
    for (i=0; i < orte_node_pool->size; i++) {
        if (NULL != opal_pointer_array_get_item(orte_node_pool, i)) {
            app->num_procs++;
        }
    }

    /* launch the virtual machine - this will launch a daemon on
     * each node, including our own
     */
    if (ORTE_SUCCESS != orte_plm.spawn(daemons)) {
        opal_output(orte_clean_output, "\nORCM DISTRIBUTED VIRTUAL MACHINE %s FAILED TO LAUNCH\n",
                    ORTE_JOB_FAMILY_PRINT(ORTE_PROC_MY_NAME->jobid));
        goto cleanup;
    }
    
    opal_output(orte_clean_output, "\nORCM DISTRIBUTED VIRTUAL MACHINE %s RUNNING...\n",
                ORTE_JOB_FAMILY_PRINT(ORTE_PROC_MY_NAME->jobid));
    
    opal_event_dispatch(opal_event_base);
    
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
    
    return orte_exit_status;
}

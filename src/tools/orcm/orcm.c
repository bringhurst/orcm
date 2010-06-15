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
static orte_job_t *daemons, *orcm;
static uint32_t my_uid;
static bool start_flag;
static opal_mutex_t start_lock;
static opal_condition_t start_cond;

static void ps_request(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buffer,
                       void *cbdata);

static void vm_tracker(char *app, char *version, char *release,
                       orte_process_name_t *name, char *node, uint32_t uid);

static void release(int fd, short flag, void *dump);

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

    my_uid = (uint32_t)getuid();
    
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

    OBJ_CONSTRUCT(&start_lock, opal_mutex_t);
    OBJ_CONSTRUCT(&start_cond, opal_condition_t);
    start_flag = true;
    
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
    
    /* need the cfgi framework too */
    if (ORTE_SUCCESS != (ret = orcm_cfgi_base_open())) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    if (ORTE_SUCCESS != (ret = orcm_cfgi_base_select())) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    /* lookup the daemon job data object */
    if (NULL == (daemons = orte_get_job_data_object(ORTE_PROC_MY_NAME->jobid))) {
        opal_output(orte_clean_output, "COULD NOT GET DAEMON JOB OBJECT");
        goto cleanup;
    }
    /* ensure the accounting starts correctly */
    daemons->num_reported = 1;
    
    /* listen for PS requests */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcm-ps", "0.1", "alpha",
                                                         ORCM_PNP_SYS_CHANNEL,
                                                         ORCM_PNP_TAG_PS,
                                                         ps_request))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    /* announce our existence - this carries with it our rml uri and
     * our local node system info
     */
    if (ORCM_SUCCESS != (ret = orcm_pnp.announce("ORCM", "0.1", "alpha", vm_tracker))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }

    /* give ourselves a second to wait for announce responses to detect
     * any pre-existing daemons/orcms that might conflict
     */
    ORTE_TIMER_EVENT(1, 0, release);
    
    OPAL_ACQUIRE_THREAD(&start_lock, &start_cond, &start_flag);
    OPAL_THREAD_UNLOCK(&start_lock);
    
    /* ensure our mapping policy will utilize any VM */
    ORTE_ADD_MAPPING_POLICY(ORTE_MAPPING_USE_VM);
    /* use byslot mapping by default */
    ORTE_ADD_MAPPING_POLICY(ORTE_MAPPING_BYSLOT);

    /* create an app */
    app = OBJ_NEW(orte_app_context_t);
    app->app = strdup("orcmd");
    opal_argv_append_nosize(&app->argv, "orcmd");
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
    
    opal_output(orte_clean_output, "\nORCM DISTRIBUTED VIRTUAL MACHINE %s RUNNING...\n",
                ORTE_JOB_FAMILY_PRINT(ORTE_PROC_MY_NAME->jobid));
    
    opal_event_dispatch();
    
    /***************
     * Cleanup
     ***************/
 cleanup:
    ORTE_UPDATE_EXIT_STATUS(orte_exit_status);
    
    OBJ_DESTRUCT(&start_lock);
    OBJ_DESTRUCT(&start_cond);
    
    /* Remove the signal handlers */
    orcm_remove_signal_handlers();
    
    /* whack any lingering session directory files from our jobs */
    orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
    
    /* cleanup the cfgi framework */
    orcm_cfgi_base_close();
    
    /* cleanup and leave */
    orcm_finalize();
    
    return orte_exit_status;
}

static void vm_tracker(char *app, char *version, char *release,
                       orte_process_name_t *name, char *nodename, uint32_t uid)
{
    orte_proc_t *proc;
    orte_node_t *node;
    int i;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                         "%s Received announcement from %s:%s:%s proc %s on node %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), app, version, release,
                         ORTE_NAME_PRINT(name), nodename));
    
    /* if this isn't something I launched, ignore it */
    if (ORTE_JOB_FAMILY(name->jobid) != ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid)) {
        /* if this is an orcmd that belongs to this user, then we have a problem */
        if ((0 == strcasecmp(app, "orcmd") || (0 == strcasecmp(app, "orcm"))) && uid == my_uid) {
            orte_show_help("help-orcm.txt", "preexisting-orcmd", true, nodename);
            goto exitout;
        }
        return;
    }
    
    /* look up this proc */
    if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(daemons->procs, name->vpid))) {
        OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                             "%s adding new daemon",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        
        /* new daemon - add it */
        proc = OBJ_NEW(orte_proc_t);
        proc->name.jobid = name->jobid;
        proc->name.vpid = name->vpid;
        daemons->num_procs++;
        opal_pointer_array_set_item(daemons->procs, name->vpid, proc);
    }
    /* ensure the state is set to running */
    proc->state = ORTE_PROC_STATE_RUNNING;
    /* if it is a restart, check the node against the
     * new one to see if it changed location
     */
    if (NULL != proc->nodename) {
        if (0 != strcmp(nodename, proc->nodename)) {
            OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                 "%s restart detected",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
            /* must have moved */
            OBJ_RELEASE(proc->node);  /* maintain accounting */
            proc->nodename = NULL;
        }
    }
    
    /* find this node in our array */
    for (i=0; i < orte_node_pool->size; i++) {
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, i))) {
            continue;
        }
        if (0 == strcmp(node->name, nodename)) {
            /* already have this node - could be a race condition
             * where the daemon died and has been replaced, so
             * just assume that is the case
             */
            if (NULL != node->daemon) {
                OBJ_RELEASE(node->daemon);
            }
            goto complete;
        }
    }
    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                         "%s adding new node",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* if we get here, this is a new node */
    node = OBJ_NEW(orte_node_t);
    node->name = strdup(nodename);
    node->state = ORTE_NODE_STATE_UP;
    node->index = opal_pointer_array_add(orte_node_pool, node);
complete:
    OBJ_RETAIN(node);  /* maintain accounting */
    proc->node = node;
    proc->nodename = node->name;
    OBJ_RETAIN(proc);  /* maintain accounting */
    node->daemon = proc;
    node->daemon_launched = true;
    /* track number we have heard from */
    daemons->num_reported++;

    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                         "%s %d of %d reported",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         daemons->num_reported, daemons->num_procs));

    /* check if we have heard from them all */
    if (daemons->num_procs <= daemons->num_reported) {
        OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                             "%s declaring launch complete",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        
        OPAL_WAKEUP_THREAD(&orte_plm_globals.spawn_in_progress_cond,
                           &orte_plm_globals.spawn_in_progress);
    }
    
    return;
    
exitout:
    /* Remove the signal handlers */
    orcm_remove_signal_handlers();
    
    /* whack any lingering session directory files from our jobs */
    orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
    
    /* cleanup and leave */
    orcm_finalize();
    
    exit(orte_exit_status);
}

static void release(int fd, short flag, void *dump)
{
    OPAL_WAKEUP_THREAD(&start_cond, &start_flag);
}

static void ps_request(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buffer,
                       void *cbdata)
{
    orte_process_name_t name;
    int32_t n;
    int rc;
    opal_buffer_t ans;
    
    /* unpack the target name */
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &name, &n, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* construct the response */
    OBJ_CONSTRUCT(&ans, opal_buffer_t);
    
    /* if the jobid is wildcard, they just want to know who is out there */
    if (ORTE_JOBID_WILDCARD == name.jobid) {
        goto respond;
    }
    
    /* if the requested job family isn't mine, and isn't my DVM, then ignore it */
    if (ORTE_JOB_FAMILY(name.jobid) != ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid)) {
        OBJ_DESTRUCT(&ans);
        return;
    }
    
    /* if the request is for my job family... */
    if (ORTE_JOB_FAMILY(name.jobid) == ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid)) {
        /* if the vpid is wildcard, then the caller wants this info from everyone.
         * if the vpid is not wildcard, then only respond if I am the leader
         */
        if (ORTE_VPID_WILDCARD == name.vpid || orcm_lowest_rank) {
            /* pack the response */
            goto pack;
        }
        /* otherwise, just ignore this */
        OBJ_DESTRUCT(&ans);
        return;
    }
    
    /* the request must have been for my DVM - if they don't want everyone to
     * respond, ignore it
     */
    if (ORTE_VPID_WILDCARD != name.vpid) {
        OBJ_DESTRUCT(&ans);
        return;
    }
    
pack:
    opal_dss.pack(&ans, ORTE_PROC_MY_NAME, 1, ORTE_NAME);
    
respond:
    if (ORCM_SUCCESS != (rc = orcm_pnp.output(ORCM_PNP_SYS_CHANNEL,
                                              NULL, ORCM_PNP_TAG_PS,
                                              NULL, 0, &ans))) {
        ORTE_ERROR_LOG(rc);
    }
    OBJ_DESTRUCT(&ans);
}

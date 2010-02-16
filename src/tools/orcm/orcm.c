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

#include "orte/runtime/runtime.h"
#include "orte/util/show_help.h"
#include "orte/util/parse_options.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"
#include "orte/runtime/orte_locks.h"
#include "orte/mca/plm/plm.h"
#include "orte/mca/plm/base/plm_private.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/grpcomm/grpcomm.h"
#include "orte/mca/rmcast/base/base.h"
#include "orte/mca/recos/recos.h"

/* ensure I can behave like a daemon */
#include "orte/orted/orted.h"

#include "mca/cfgi/cfgi.h"

/*****************************************
 * Global Vars for Command line Arguments
 *****************************************/
static struct {
    bool help;
    char *config_file;
    int verbosity;
    char *hostfile;
    char *hosts;
    char *report_uri;
} my_globals;

opal_cmd_line_init_t cmd_line_opts[] = {
    { NULL, NULL, NULL, 
      'h', NULL, "help", 
      0,
      &my_globals.help, OPAL_CMD_LINE_TYPE_BOOL,
      "Print help message" },

    { NULL, NULL, NULL, 'd', "debug", "debug", 1,
      &my_globals.verbosity, OPAL_CMD_LINE_TYPE_INT,
      "Debug verbosity (default: 0)" },
    
    { NULL, NULL, NULL, '\0', "boot", "boot", 1,
      &my_globals.config_file, OPAL_CMD_LINE_TYPE_STRING,
      "Name of file containing the boot configuration - also indicates that this CM is to boot the system" },
    
    { NULL, NULL, NULL, 'h', "hostfile", "hostfile", 1,
      &my_globals.hostfile, OPAL_CMD_LINE_TYPE_STRING,
      "Name of file containing hosts upon which daemons are to be started" },
    
    { NULL, NULL, NULL, 'H', "host", "host", 1,
      &my_globals.hosts, OPAL_CMD_LINE_TYPE_STRING,
      "Comma-delimited list of hosts upon which daemons are to be started" },
    
    { "rmaps", "base", "no_schedule_local", '\0', "nolocal", "nolocal", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Do not run any applications on the local node" },

    { NULL, NULL, NULL, '\0', "report-uri", "report-uri", 1,
      &my_globals.report_uri, OPAL_CMD_LINE_TYPE_STRING,
      "Printout URI on stdout [-], stderr [+], or a file [anything else]" },

    /* End of list */
    { NULL, NULL, NULL, '\0', NULL, NULL, 0,
      NULL, OPAL_CMD_LINE_TYPE_NULL,
      NULL }
};

/*
 * Local variables & functions
 */
static struct opal_event term_handler;
static struct opal_event int_handler;
static opal_event_t *orted_exit_event, *cm_exit_event;
static int num_apps=0;
static bool forcibly_die = false;

static void abort_signal_callback(int fd, short flags, void *arg);
static void abort_exit_callback(int fd, short flags, void *arg);
static void job_complete(int fd, short flags, void *arg);
static void just_quit(int fd, short ign, void *arg);

static void spawn_app(int fd, short event, void *command);
static int kill_app(char *cmd, opal_value_array_t *replicas);

static void recv_boot_req(int status, orte_process_name_t* sender,
                          opal_buffer_t *buffer, orte_rml_tag_t tag,
                          void* cbdata);

static void recv_bootstrap(int status, orte_process_name_t* sender,
                           opal_buffer_t *buffer, orte_rml_tag_t tag,
                           void* cbdata);
static char* regen_uri(char *old_uri, orte_process_name_t *name);

#if ORTE_ENABLE_MULTICAST
static void daemon_announce(int status,
                            orte_rmcast_channel_t channel,
                            orte_rmcast_tag_t tag,
                            orte_process_name_t *sender,
                            opal_buffer_t *buf, void *cbdata);

static int setup_daemon(orte_process_name_t *name,
                        char *nodename, char *rml_uri,
                        opal_buffer_t *buf);

static void ps_recv(int status,
                    orte_rmcast_channel_t channel,
                    orte_rmcast_tag_t tag,
                    orte_process_name_t *sender,
                    opal_buffer_t *buf, void *cbdata);
#endif

#define CM_MAX_LINE_LENGTH  1024

static char *cm_getline(FILE *fp)
{
    char *ret, *buff;
    char input[CM_MAX_LINE_LENGTH];
    
retry:
    ret = fgets(input, CM_MAX_LINE_LENGTH, fp);
    if (NULL != ret) {
        if ('#' == input[0]) {
            /* ignore this line - it is a comment */
            goto retry;
        }
        input[strlen(input)-1] = '\0';  /* remove newline */
        buff = strdup(input);
        return buff;
    }
    
    return NULL;
}


int main(int argc, char *argv[])
{
    int ret, i;
    opal_cmd_line_t cmd_line;
    char *cmd;
    orte_proc_state_t state;
    orte_job_t *daemons;
    orte_app_context_t *app;
    
    /***************
     * Initialize
     ***************/
    
    /* initialize the globals */
    my_globals.help = false;
    my_globals.config_file = NULL;
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
    
    /* setup the exit triggers */
    OBJ_CONSTRUCT(&orte_exit, orte_trigger_event_t);
    OBJ_CONSTRUCT(&orteds_exit, orte_trigger_event_t);

    /* save the environment for launch purposes. This MUST be
     * done so that we can pass it to any local procs we
     * spawn - otherwise, those local procs won't see any
     * non-MCA envars were set in the enviro prior to calling
     * orterun
     */
    orte_launch_environ = opal_argv_copy(environ);
    
    /* ensure we select the orcm recos module */
    putenv("OMPI_MCA_recos=orcm");
    
    /***************************
     * We need all of OPAL and ORTE
     ***************************/
    if (ORTE_SUCCESS != orcm_init(OPENRCM_MASTER)) {
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
                exit(0);
            }
            fprintf(fp, "%s\n", (NULL == rml_uri) ? "NULL" : rml_uri);
            fclose(fp);
        }
        if (NULL != rml_uri) {
            free(rml_uri);
        }        
    }

    /* setup a non-blocking recv in case someone wants to
     * contact us and request we boot something
     */
    ret = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD, ORTE_RML_TAG_TOOL,
                                  ORTE_RML_NON_PERSISTENT, recv_boot_req, NULL);
    if (ret != ORTE_SUCCESS) {
        ORTE_ERROR_LOG(ret);
        orcm_finalize();
        return 1;
    }
    
    /* setup a non-blocking recv to catch bootstrapping daemons */
    ret = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD, ORTE_RML_TAG_BOOTSTRAP,
                                  ORTE_RML_NON_PERSISTENT, recv_bootstrap, NULL);
    if (ret != ORTE_SUCCESS) {
        ORTE_ERROR_LOG(ret);
        orcm_finalize();
        return 1;
    }
    
    /* setup the orted cmd line options to direct the
     * proper framework selections
     */
    opal_argv_append_nosize(&orted_cmd_line, "-mca");
    opal_argv_append_nosize(&orted_cmd_line, "routed");
    opal_argv_append_nosize(&orted_cmd_line, "cm");

    if (ORTE_SUCCESS != (ret = orte_wait_event(&orted_exit_event, &orteds_exit, "orted_exit", just_quit))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }

    if (ORTE_SUCCESS != (ret = orte_wait_event(&cm_exit_event, &orte_exit, "cm_exit", job_complete))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    /** setup callbacks for abort signals - from this point
     * forward, we need to abort in a manner that allows us
     * to cleanup
     */
    opal_signal_set(&term_handler, SIGTERM,
                    abort_signal_callback, &term_handler);
    opal_signal_add(&term_handler, NULL);
    opal_signal_set(&int_handler, SIGINT,
                    abort_signal_callback, &int_handler);
    opal_signal_add(&int_handler, NULL);

    /* setup a recv so we can "hear" daemons as they startup */
    if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(ORTE_RMCAST_SYS_CHANNEL,
                                                          ORTE_RMCAST_TAG_BOOTSTRAP,
                                                          ORTE_RMCAST_PERSISTENT,
                                                          daemon_announce, NULL))) {
        ORTE_ERROR_LOG(ret);
        orte_trigger_event(&orteds_exit);
    }

    /* setup a recv for ps data requests */
    if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(ORTE_RMCAST_SYS_CHANNEL,
                                                          ORTE_RMCAST_TAG_PS,
                                                          ORTE_RMCAST_PERSISTENT,
                                                          ps_recv, NULL))) {
        ORTE_ERROR_LOG(ret);
        orte_trigger_event(&orteds_exit);
    }
    
    /* lookup the daemon job data object */
    if (NULL == (daemons = orte_get_job_data_object(ORTE_PROC_MY_NAME->jobid))) {
        opal_output(orte_clean_output, "COULD NOT GET DAEMON JOB OBJECT");
        orte_trigger_event(&orteds_exit);
    }
    
    /* if we were given hosts to startup, create an app for the
     * daemon job so it can start the virtual machine
     */
    if (NULL != my_globals.hosts || NULL != my_globals.hostfile) {
        /* create an app */
        app = OBJ_NEW(orte_app_context_t);
        app->app = strdup("ORCM");
        if (NULL != my_globals.hosts) {
            app->dash_host = opal_argv_split(my_globals.hosts, ',');
        }
        if (NULL != my_globals.hostfile) {
            app->hostfile = strdup(my_globals.hostfile);
        }
        /* add it to the daemon job */
        opal_pointer_array_add(daemons->apps, app);
        daemons->num_apps = 1;
        /* ensure our launched daemons do not bootstrap */
        orte_daemon_bootstrap = false;
        /* ensure we always utilize the local node as well */
        orte_hnp_is_allocated = true;
    }
    
    /* set the mapping policy to byslot */
    ORTE_SET_MAPPING_POLICY(ORTE_MAPPING_BYSLOT);

    /* launch the virtual machine - this will launch a daemon on
     * each node. It will simply return if there are no known
     * nodes other than the one we are on
     */
    orte_plm.spawn(daemons);
    
    opal_output(orte_clean_output, "\nCLUSTER MANAGER RUNNING...\n");

    /* tell the configuration interface to read our
     * configuration, if available, and spawn it
     */
    orcm_cfgi.read_config(spawn_app);
    
    /* just wait until the abort is fired */
    opal_event_dispatch();

    /***************
     * Cleanup
     ***************/
 cleanup:
    ORTE_UPDATE_EXIT_STATUS(orte_exit_status);
    just_quit(0,0,NULL);
    return orte_exit_status;
}

static void just_quit(int fd, short ign, void *arg)
{
    /* if the orted exit event is set, delete it */
    if (NULL != orted_exit_event) {
        opal_evtimer_del(orted_exit_event);
        free(orted_exit_event);
    }
    
    /* Remove the TERM and INT signal handlers */
    opal_signal_del(&term_handler);
    opal_signal_del(&int_handler);
    
    /* whack any lingering session directory files from our jobs */
    orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
    
    /* cleanup and leave */
    orcm_finalize();
    
    exit(orte_exit_status);
}

/*
 * Attempt to terminate the job and wait for callback indicating
 * the job has been aborted.
 */
static void abort_signal_callback(int fd, short flags, void *arg)
{
    /* if we have already ordered this once, don't keep
     * doing it to avoid race conditions
     */
    if (!opal_atomic_trylock(&orte_abort_inprogress_lock)) { /* returns 1 if already locked */
        if (forcibly_die) {
            /* kill any local procs */
            orte_odls.kill_local_procs(NULL, false);
            
            /* whack any lingering session directory files from our jobs */
            orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
            
            /* exit with a non-zero status */
            exit(ORTE_ERROR_DEFAULT_EXIT_CODE);
        }
        opal_output(0, "open-cm: abort is already in progress...hit ctrl-c again to forcibly terminate\n\n");
        forcibly_die = true;
        return;
    }
    
    /* set the global abnormal exit flag so we know not to
     * use the standard xcast for terminating orteds
     */
    orte_abnormal_term_ordered = true;
    /* ensure that the forwarding of stdin stops */
    orte_job_term_ordered = true;
    
    /* We are in an event handler; the exit procedure
     * will delete the signal handler that is currently running
     * (which is a Bad Thing), so we can't call it directly.
     * Instead, we have to exit this handler and setup to call
     * abort_exit_callback after this.
     */
    ORTE_TIMER_EVENT(0, 0, abort_exit_callback);
}

static void abort_exit_callback(int fd, short ign, void *arg)
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
        just_quit(fd, ign, arg);
    }
    /* ensure all the orteds depart together */
    orte_grpcomm.onesided_barrier();

    ORTE_UPDATE_EXIT_STATUS(ret);
    just_quit(0, 0, NULL);
}

static void job_complete(int fd, short ign, void *arg)
{
    orte_trigger_event_t *trig = (orte_trigger_event_t*)arg;
    int ret;
    
    /* we don't really care that all known jobs have
     * completed - we want to stay alive until
     * someone kills us
     */
    opal_output(orte_clean_output, "\nCLUSTER MANAGER IDLE...\n");
    
    /* reset the trigger event so we can be called again */
    
    free(cm_exit_event);
    OBJ_DESTRUCT(&orte_exit);
    OBJ_CONSTRUCT(&orte_exit, orte_trigger_event_t);
    if (ORTE_SUCCESS != (ret = orte_wait_event(&cm_exit_event, &orte_exit, "cm_exit", job_complete))) {
        ORTE_ERROR_LOG(ret);
    }
}

static void spawn_app(int fd, short event, void *command)
{
    orcm_spawn_event_t *spev = (orcm_spawn_event_t*)command;
    char *cmd = spev->cmd;
    int np = spev->np;
    char *hosts = spev->hosts;
    bool constrain = spev->constrain;
    int ret, i, n;
    orte_job_t *jdata;
    orte_proc_t *proc;
    orte_app_context_t *app;
    char *param, *value;
    orte_proc_state_t state;
    char cwd[OPAL_PATH_MAX];
    char **inpt;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                         "%s spawn:app: %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), cmd));
    
    /* if we are adding procs, find the existing job object */
    if (spev->add_procs) {
        OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                             "%s spawn: adding application",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        for (i=0; i < orte_job_data->size; i++) {
            if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
                continue;
            }
            if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, 0))) {
                continue;
            }
            if (0 == strcmp(cmd, app->app)) {
                /* found it */
                OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                     "%s spawn: found job %s - adding %d proc(s)",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_JOBID_PRINT(jdata->jobid), np));
                /* add the required number of proc objects to the jdata object */
                for (n=0; n < np; n++) {
                    proc = OBJ_NEW(orte_proc_t);
                    proc->name.jobid = jdata->jobid;
                    proc->name.vpid = jdata->num_procs++;
                    proc->app_idx = app->idx;
                    proc->state = ORTE_PROC_STATE_RESTART;
                    opal_pointer_array_set_item(jdata->procs, proc->name.vpid, proc);
                }
                /* increment num procs */
                app->num_procs += np;
                /* set the state to restart so we don't think it's a new job */
                jdata->state = ORTE_JOB_STATE_RESTART;
                goto launch;
            }
        }
    }
    /* get here if we are not adding procs, or we couldn't find the
     * pre-existing job object
     */
    
    /* identify the states where we want callbacks */
    state = ORTE_PROC_STATE_ABORTED | ORTE_PROC_STATE_FAILED_TO_START |
            ORTE_PROC_STATE_ABORTED_BY_SIG | ORTE_PROC_STATE_TERM_WO_SYNC;
    
    /* create a new job for this app */
    jdata = OBJ_NEW(orte_job_t);
    /* break the cmd line down */
    inpt = opal_argv_split(cmd, ' ');
    /* setup the required info */
    app = OBJ_NEW(orte_app_context_t);
    app->app = strdup(inpt[0]);
    opal_argv_append_nosize(&app->argv, inpt[0]);
    /* copy any args */
    for (i=1; NULL != inpt[i]; i++) {
        opal_argv_append_nosize(&app->argv, inpt[i]);
    }
    /* done with this */
    opal_argv_free(inpt);
    
    /* get the cwd */
    if (OPAL_SUCCESS != opal_getcwd(cwd, sizeof(cwd))) {
        opal_output(0, "failed to get cwd");
        return;
    }
    app->cwd = strdup(cwd);
    app->num_procs = np;
    app->prefix_dir = strdup(opal_install_dirs.prefix);
    /* setup the hosts */
    if (NULL != hosts) {
        app->dash_host = opal_argv_split(hosts, ' ');
    }
    for (i = 0; NULL != environ[i]; ++i) {
        if (0 == strncmp("OMPI_", environ[i], 5)) {
            /* check for duplicate in app->env - this
             * would have been placed there by the
             * cmd line processor. By convention, we
             * always let the cmd line override the
             * environment
             */
            param = strdup(environ[i]);
            value = strchr(param, '=');
            *value = '\0';
            value++;
            opal_setenv(param, value, false, &app->env);
            free(param);
        }
    }
    /* assign this group of apps a multicast group */
    asprintf(&value, "%s:%d", app->app, (num_apps+ORTE_RMCAST_DYNAMIC_CHANNELS));
    opal_setenv("OMPI_MCA_rmcast_base_group", value, true, &app->env);
    free(value);
    num_apps++;
    
    opal_pointer_array_add(jdata->apps, app);
    jdata->num_apps = 1;
    /* indicate that this is to be a continuously operating job - i.e.,
     * don't wake us up and terminate us if all of this job's
     * procs terminate
     */
    jdata->controls |= ORTE_JOB_CONTROL_CONTINUOUS_OP;
    /* pass max number of restarts */
    jdata->max_restarts = spev->max_restarts;
    
launch:
    /* if we want to debug the apps, set the proper envar */
    if (spev->debug) {
        opal_setenv("ORCM_MCA_spin", "1", true, &app->env);
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                         "%s LAUNCHING APP %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), app->app));
    /* spawn it */
    if (ORTE_SUCCESS != (ret = orte_plm.spawn(jdata))) {
        opal_output(0, "FAILED TO SPAWN APP %s", app->app);
        return;
    }
    /* ensure we allow the system to respawn anywhere, unless user
     * constrained us
     */
    if (constrain && NULL != app->dash_host) {
        opal_argv_free(app->dash_host);
        app->dash_host = NULL;
    }
    /* ORTE will cleanup and release the spawndata object */
    return;
}

static int kill_app(char *cmd, opal_value_array_t *replicas)
{
    int i, j;
    orte_job_t *jdata;
    orte_app_context_t *app;
    char **inpt;
    orte_proc_t *proc;
    size_t n, num_reps;
    opal_pointer_array_t procs;
    bool killall;
    
    num_reps = opal_value_array_get_size(replicas);
    if (0 == num_reps || -1 == OPAL_VALUE_ARRAY_GET_ITEM(replicas, int32_t, 0)) {
        killall = true;
    } else {
        killall = false;
        OBJ_CONSTRUCT(&procs, opal_pointer_array_t);
        opal_pointer_array_init(&procs, 16, INT_MAX, 16);
        opal_pointer_array_set_size(&procs, num_reps);
        for (j=0; j < num_reps; j++) {
            proc = OBJ_NEW(orte_proc_t);
            proc->name.vpid = OPAL_VALUE_ARRAY_GET_ITEM(replicas, int32_t, j);
            opal_pointer_array_set_item(&procs, j, proc);
        }
    }
    
    /* break the input line down */
    inpt = opal_argv_split(cmd, ' ');
    for (i = 0; NULL != inpt[i]; ++i) {
        OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                             "%s kill:app: %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), inpt[i]));
        /* find all job data objects for this app - skip the daemon job
         * We have to check all the jobs because there could be multiple
         * invocations of the same application
         */
        for (j=1; j < orte_job_data->size; j++) {
            if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, j))) {
                continue;
            }
            if (jdata->state > ORTE_PROC_STATE_UNTERMINATED) {
                /* job is already terminated */
                continue;
            }
            /* retrieve the app */
            if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, 0))) {
                /* youch - this won't work */
                ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
                return ORTE_ERR_NOT_FOUND;
            }
            if (0 == strcasecmp(inpt[i], app->app)) {
                /* if no replicas were provided, or if the wildcard
                 * was provided, kill the entire job
                 */
                if (killall) {
                    /* turn off the restart for this job */
                    orte_recos.detach_job(jdata);
                    /* indicate that this is no longer a continuously operating job */
                    jdata->controls &= ~ORTE_JOB_CONTROL_CONTINUOUS_OP;
                    /* kill this job */
                    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                         "%s killing job %s app: %s",
                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                         ORTE_JOBID_PRINT(jdata->jobid), inpt[i]));
                    orte_plm.terminate_job(jdata->jobid);
                } else {
                    /* kill the individual procs */
                    for (j=0; j < num_reps; j++) {
                        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(&procs, j))) {
                            continue;
                        }
                        proc->name.jobid = jdata->jobid;
                        OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                             "%s killing proc %s",
                                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                             ORTE_NAME_PRINT(&proc->name)));
                    }
                    orte_plm.terminate_procs(&procs);
                }
                continue;
            }
        }
    }

    if (!killall) {
        for (j=0; j < num_reps; j++) {
            if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(&procs, j))) {
                continue;
            }
            OBJ_RELEASE(proc);
        }
        OBJ_DESTRUCT(&procs);
    }

    return ORTE_SUCCESS;
}

static void process_boot_req(int fd, short event, void *data)
{
    orte_message_event_t *mev = (orte_message_event_t*)data;
    opal_buffer_t *buffer = mev->buffer;
    char *cmd, *hosts;
    int32_t rc, n, j, num_apps, replica, restarts;
    opal_buffer_t response;
    orte_job_t *jdata;
    orte_app_context_t *app;
    orte_proc_t *proc;
    orte_vpid_t vpid;
    orcm_tool_cmd_t flag;
    int8_t constrain, add_procs, debug;
    opal_value_array_t replicas;
    
    /* unpack the cmd type */
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &flag, &n, OPENRCM_TOOL_CMD_T))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    if (OPENRCM_TOOL_PS_CMD == flag) {
        OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                             "%s ps cmd from %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&mev->sender)));
        OBJ_CONSTRUCT(&response, opal_buffer_t);
        vpid = ORTE_VPID_INVALID;
        for (n=0; n < orte_job_data->size; n++) {
            if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, n))) {
                continue;
            }
            if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, 0))) {
                continue;
            }
            opal_dss.pack(&response, &app->app, 1, OPAL_STRING);
            for (j=0; j < jdata->procs->size; j++) {
                if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, j))) {
                    continue;
                }
                opal_dss.pack(&response, &(proc->name.vpid), 1, ORTE_VPID);
                opal_dss.pack(&response, &proc->nodename, 1, OPAL_STRING);
            }
            /* flag the end for this job */
            opal_dss.pack(&response, &vpid, 1, ORTE_VPID);
        }
        if (0 > (rc = orte_rml.send_buffer(&mev->sender, &response, ORTE_RML_TAG_TOOL, 0))) {
            ORTE_ERROR_LOG(rc);
        }
        OBJ_DESTRUCT(&response);
        goto cleanup;
    }
    
    if (OPENRCM_TOOL_START_CMD == flag) {
        OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                             "%s spawn cmd from %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&mev->sender)));
        /* unpack the add procs flag */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &add_procs, &n, OPAL_INT8))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the debug flag */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &debug, &n, OPAL_INT8))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the max number of restarts */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &restarts, &n, OPAL_INT32))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the #instances to start */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &num_apps, &n, OPAL_INT32))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the starting hosts - okay to unpack a NULL string */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &hosts, &n, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the constrain flag */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &constrain, &n, OPAL_INT8))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the cmd */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &cmd, &n, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* spawn it */
        OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                             "%s spawning cmd %s np %d hosts %s constrain %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             cmd, num_apps,
                             (NULL == hosts) ? "NULL" : hosts,
                             (0 == constrain) ? "FALSE" : "TRUE"));
        ORCM_SPAWN_EVENT(cmd, add_procs, debug, restarts, num_apps, hosts, constrain, spawn_app);
    } else if (OPENRCM_TOOL_STOP_CMD == flag) {
        /* setup a replica array */
        OBJ_CONSTRUCT(&replicas, opal_value_array_t);
        opal_value_array_init(&replicas, sizeof(int32_t));
        /* unpack the number of replicas included in command */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &num_apps, &n, OPAL_INT32))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        opal_value_array_set_size(&replicas, (size_t)num_apps);
        /* unpack the replica info */
        for (j=0; j < num_apps; j++) {
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &replica, &n, OPAL_INT32))) {
                ORTE_ERROR_LOG(rc);
                goto cleanup;
            }
            OPAL_VALUE_ARRAY_SET_ITEM(&replicas, int32_t, j, replica);
        }
        n=1;
        while (ORTE_SUCCESS == (rc = opal_dss.unpack(buffer, &cmd, &n, OPAL_STRING))) {
            OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                 "%s kill cmd from %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(&mev->sender)));
            if (ORTE_SUCCESS != (rc = kill_app(cmd, &replicas))) {
                break;
            }
            n=1;
        }
        if (ORTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            ORTE_ERROR_LOG(rc);
        } else {
            rc = ORTE_SUCCESS;
        }
        /* destruct the replica array */
        OBJ_DESTRUCT(&replicas);
    } else {
        opal_output(0, "%s: UNKNOWN TOOL CMD FLAG %d", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)flag);
        goto cleanup;
    }

    OBJ_CONSTRUCT(&response, opal_buffer_t);
    opal_dss.pack(&response, &rc, 1, OPAL_INT32);
    if (0 > (rc = orte_rml.send_buffer(&mev->sender, &response, ORTE_RML_TAG_TOOL, 0))) {
        ORTE_ERROR_LOG(rc);
    }
    OBJ_DESTRUCT(&response);

cleanup:
    /* release the message object */
    OBJ_RELEASE(mev);
}

static void recv_boot_req(int status, orte_process_name_t* sender,
                          opal_buffer_t *buffer, orte_rml_tag_t tag,
                          void* cbdata)
{
    int rc;
    
    OPAL_OUTPUT_VERBOSE((5, orcm_debug_output,
                         "%s tool cmd recvd from %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));
    
    /* don't process this right away - we need to get out of the recv before
     * we process the message as it may ask us to do something that involves
     * more messaging! Instead, setup an event so that the message gets processed
     * as soon as we leave the recv.
     *
     * The macro makes a copy of the buffer, which we release when processed - the incoming
     * buffer, however, is NOT released here, although its payload IS transferred
     * to the message buffer for later processing
     */
    ORTE_MESSAGE_EVENT(sender, buffer, tag, process_boot_req);
    
    /* reissue the recv */
    rc = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD, ORTE_RML_TAG_TOOL,
                                  ORTE_RML_NON_PERSISTENT, recv_boot_req, NULL);
    if (rc != ORTE_SUCCESS) {
        ORTE_ERROR_LOG(rc);
    }    
}

static void process_bootstrap(int fd, short event, void *data)
{
    orte_message_event_t *mev = (orte_message_event_t*)data;
    opal_buffer_t *buffer = mev->buffer;
    char *rml_uri = NULL;
    int rc, idx;
    orte_proc_t *daemon;
    orte_job_t *jdatorted;
    char *nodename=NULL;
    
    /* get the orted job data object */
    if (NULL == (jdatorted = orte_get_job_data_object(ORTE_PROC_MY_NAME->jobid))) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        goto cleanup;
    }
    
    /* unpack its contact info */
    idx = 1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &rml_uri, &idx, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* set the contact info into the hash table */
    if (ORTE_SUCCESS != (rc = orte_rml.set_contact_info(rml_uri))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* unpack the node it is on */
    idx = 1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &nodename, &idx, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    OPAL_OUTPUT_VERBOSE((5, orcm_debug_output,
                         "%s report ready from daemon %s on node %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(&mev->sender), nodename));
    
    /* setup this daemon */
    setup_daemon(&mev->sender, nodename, rml_uri, buffer);
    
cleanup:
    if (NULL != nodename) {
        free(nodename);
    }
    /* release the message object */
    OBJ_RELEASE(mev);
}

static void recv_bootstrap(int status, orte_process_name_t* sender,
                           opal_buffer_t *buffer, orte_rml_tag_t tag,
                           void* cbdata)
{
    int rc;
    
    OPAL_OUTPUT_VERBOSE((5, orcm_debug_output,
                         "%s bootstrap recvd from %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));

    /* don't process this right away - we need to get out of the recv before
     * we process the message as it may ask us to do something that involves
     * more messaging! Instead, setup an event so that the message gets processed
     * as soon as we leave the recv.
     *
     * The macro makes a copy of the buffer, which we release when processed - the incoming
     * buffer, however, is NOT released here, although its payload IS transferred
     * to the message buffer for later processing
     */
    ORTE_MESSAGE_EVENT(sender, buffer, tag, process_bootstrap);
    
    /* reissue the recv */
    rc = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD, ORTE_RML_TAG_BOOTSTRAP,
                                 ORTE_RML_NON_PERSISTENT, recv_bootstrap, NULL);
    if (rc != ORTE_SUCCESS) {
        ORTE_ERROR_LOG(rc);
    }    
}

static void daemon_announce(int status,
                            orte_rmcast_channel_t channel,
                            orte_rmcast_tag_t tag,
                            orte_process_name_t *sender,
                            opal_buffer_t *buf, void *cbdata)
{
    int32_t n;
    int rc;
    char *nodename;
    orte_daemon_cmd_flag_t cmd;
    opal_buffer_t answer;
    orte_process_name_t name;
    int i;
    char *uri;
    
    /* unpack the cmd */
    n = 1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &cmd, &n, ORTE_DAEMON_CMD_T))) {
        ORTE_ERROR_LOG(rc);
        goto depart;
    }
    
    /* construct the answer */
    OBJ_CONSTRUCT(&answer, opal_buffer_t);
    /* ack the cmd back so the sender knows what response to expect */
    opal_dss.pack(&answer, &cmd, 1, ORTE_DAEMON_CMD_T);
    
    switch(cmd) {
        case ORTE_DAEMON_NAME_REQ_CMD:
            OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                 "%s daemon name req recvd",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
            /* unpack the node it is on */
            n = 1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &nodename, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                goto depart;
            }
            /* unpack its uri */
            n = 1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &uri, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                goto depart;
            }
            name.jobid = ORTE_JOBID_INVALID;
            name.vpid = ORTE_VPID_INVALID;
            setup_daemon(&name, nodename, uri, buf);
            free(uri);
            OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                 "%s returning name %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(&name)));
            /* pack the nodename of the recipient */
            opal_dss.pack(&answer, &nodename, 1, OPAL_STRING);
            /* pack the name and my uri */
            opal_dss.pack(&answer, &name, 1, ORTE_NAME);
            uri = orte_rml.get_contact_info();
            opal_dss.pack(&answer, &uri, 1, OPAL_STRING);
            free(uri);
            /* tell it how many daemons exist */
            n = orte_process_info.num_procs;
            opal_dss.pack(&answer, &n, 1, OPAL_INT32);
            break;
        case ORTE_DAEMON_CHECKIN_CMD:
            /* unpack the node it is on */
            n = 1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &nodename, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                goto depart;
            }
            OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                 "%s daemon %s checked in from node %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(sender), nodename));
            setup_daemon(sender, nodename, NULL, buf);
            /* pack the nodename of the recipient */
            opal_dss.pack(&answer, &nodename, 1, OPAL_STRING);
            /* return our uri info */
            uri = orte_rml.get_contact_info();
            opal_dss.pack(&answer, &uri, 1, OPAL_STRING);
            free(uri);
            /* tell it how many daemons exist */
            n = orte_process_info.num_procs;
            opal_dss.pack(&answer, &n, 1, OPAL_INT32);
            break;
        case ORTE_TOOL_CHECKIN_CMD:
            /* unpack the node it is on */
            n = 1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &nodename, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                goto depart;
            }
            OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                 "%s tool %s checked in from node %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(sender), nodename));
            /* tools assign their own names, so all we need to
             * send back is our contact info
             */
            uri = orte_rml.get_contact_info();
            opal_dss.pack(&answer, &uri, 1, OPAL_STRING);
            free(uri);
            break;

        default:
            ORTE_ERROR_LOG(ORTE_ERR_NOT_SUPPORTED);
            goto depart;
            break;
    }
    
depart:
    /* always send back a response so the caller doesn't hang */
    if (ORTE_SUCCESS != (rc = orte_rmcast.send_buffer(ORTE_RMCAST_SYS_CHANNEL,
                                                      ORTE_RMCAST_TAG_BOOTSTRAP,
                                                      &answer))) {
        ORTE_ERROR_LOG(rc);
    }
    OBJ_DESTRUCT(&answer);
    
    return;
}

static orte_proc_t* add_daemon(orte_job_t *daemons, orte_node_t *node)
{
    orte_proc_t *proc;
    int rc;
    
    proc = OBJ_NEW(orte_proc_t);
    proc->name.jobid = ORTE_PROC_MY_NAME->jobid;
    proc->name.vpid = daemons->num_procs;
    proc->node = node;
    proc->nodename = node->name;
    /* add the daemon to the daemon job object */
    if (0 > (rc = opal_pointer_array_add(daemons->procs, (void*)proc))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(proc);
        return NULL;
    }
    ++daemons->num_procs;
    /* point the node to the daemon */
    node->daemon = proc;
    OBJ_RETAIN(proc);  /* maintain accounting */
    /* ensure I know the correct count */
    orte_process_info.num_procs++;
    return proc;
}

static int setup_daemon(orte_process_name_t *name,
                        char *nodename, char *rml_uri,
                        opal_buffer_t *buf)
{
    orte_node_t *node, *nd;
    orte_proc_t *proc;
    orte_job_t *daemons;
    int i, rc, n;
    int32_t num_values;
    opal_sysinfo_value_t *info;
    opal_list_item_t *item;
    char *new_uri;
    
    /* find the node */
    node = NULL;
    for (i=0; i < orte_node_pool->size; i++) {
        if (NULL == (nd = opal_pointer_array_get_item(orte_node_pool, i))) {
            continue;
        }
        if (0 == strcmp(nodename, nd->name)) {
            opal_output(0, "found node %s nodename %s daemon %s", nd->name, nodename,
                        (NULL == nd->daemon) ? "NULL" : ORTE_NAME_PRINT(&(nd->daemon->name)));
            node = nd;
            break;
        }
    }
    if (NULL == node) {
        /* new node - add it */
        OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                             "%s adding new node %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), nodename));
        node = OBJ_NEW(orte_node_t);
        node->name = strdup(nodename);
        /* we have no idea how many slots might be there,
         * so just fake it out for now
         */
        node->slots_alloc = 1;
        node->slots_max = 0;
        /* insert it into the array */
        node->index = opal_pointer_array_add(orte_node_pool, (void*)node);
        if (0 > (rc = node->index)) {
            ORTE_ERROR_LOG(rc);
            return rc;
        }
    }
    
    /* if node resource info was provided, add/update it */
    n=1;
    if (OPAL_SUCCESS == opal_dss.unpack(buf, &num_values, &n, OPAL_INT32) &&
        0 < num_values) {
        /* clear the old list, if it exists */
        while (NULL != (item = opal_list_remove_first(&node->resources))) {
            OBJ_RELEASE(item);
        }
        for (i=0; i < num_values; i++) {
            info = OBJ_NEW(opal_sysinfo_value_t);
            n=1;
            opal_dss.unpack(buf, &info->key, &n, OPAL_STRING);
            n=1;
            opal_dss.unpack(buf, &info->type, &n, OPAL_DATA_TYPE_T);
            n=1;
            if (OPAL_INT64 == info->type) {
                opal_dss.unpack(buf, &(info->data.i64), &n, OPAL_INT64);
            } else if (OPAL_STRING == info->type) {
                opal_dss.unpack(buf, &(info->data.str), &n, OPAL_STRING);
            }
            OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                 "%s adding resource %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 info->key));
            opal_list_append(&node->resources, &info->super);
        }
    }
    
    /* get the daemon job data struct */
    if (NULL == (daemons = orte_get_job_data_object(ORTE_PROC_MY_NAME->jobid))) {
        /* bad news */
        ORTE_ERROR_LOG(ORTE_ERR_FATAL);
        return ORTE_ERR_FATAL;
    }

    /* if the name is invalid, then we need to assign one */
    if (ORTE_JOBID_INVALID == name->jobid &&
        ORTE_VPID_INVALID == name->vpid) {
        OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                             "%s assigning daemon name",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        /* see if we already have a daemon recorded for
         * this node - could be restart
         */
        if (NULL != node->daemon) {
            /* it was! is this daemon still tagged as alive? */
            if (node->daemon->state < ORTE_PROC_STATE_UNTERMINATED) {
                ORTE_ERROR_LOG(ORTE_ERR_FATAL);
                return ORTE_ERR_FATAL;
            }
            /* nope - must be restart, pass name back */
            name->jobid = node->daemon->name.jobid;
            name->vpid = node->daemon->name.vpid;
            OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                 "%s daemon %s is restarting",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(name)));
            /* the old rml contact info cannot be correct, so
             * clear it out
             */
            if (NULL != node->daemon->rml_uri) {
                free(node->daemon->rml_uri);
                node->daemon->rml_uri = NULL;
            }
            /* update its rml contact info, if provided */
            if (NULL != rml_uri) {
                /* get it with the new name */
                if (NULL == (new_uri = regen_uri(rml_uri, name))) {
                    ORTE_ERROR_LOG(ORTE_ERR_FATAL);
                    return ORTE_ERR_FATAL;
                }
                node->daemon->rml_uri = strdup(new_uri);
                /* update in the rml too! */
                OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                     "%s updating daemon %s rml to %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_NAME_PRINT(name), new_uri));
                if (ORTE_SUCCESS != (rc = orte_rml.set_contact_info(new_uri))) {
                    ORTE_ERROR_LOG(rc);
                    free(new_uri);
                    return(rc);
                }                
                free(new_uri);
            }
        } else {
            /* no daemon was previously defined for it. make one */
            if (NULL == (proc = add_daemon(daemons, node))) {
                return ORTE_ERR_FATAL;
            }
            name->jobid = proc->name.jobid;
            name->vpid = proc->name.vpid;
            if (NULL != rml_uri) {
                /* get it with the new name */
                if (NULL == (new_uri = regen_uri(rml_uri, name))) {
                    ORTE_ERROR_LOG(ORTE_ERR_FATAL);
                    return ORTE_ERR_FATAL;
                }
                proc->rml_uri = strdup(new_uri);
                /* update in the rml too! */
                OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                     "%s updating daemon %s rml to %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_NAME_PRINT(name), new_uri));
                if (ORTE_SUCCESS != (rc = orte_rml.set_contact_info(new_uri))) {
                    ORTE_ERROR_LOG(rc);
                    free(new_uri);
                    return(rc);
                }
                free(new_uri);
            }
            OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                 "%s add daemon %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(name)));
        }
        return ORTE_SUCCESS;
    }

    /* this daemon already has a name that it probably computed
     * itself - see if this daemon already exists in our data
     */
    for (i=0; i < daemons->procs->size; i++) {
        if (NULL == (proc = opal_pointer_array_get_item(daemons->procs, i))) {
            continue;
        }
        if (name->jobid == proc->name.jobid &&
            name->vpid == proc->name.vpid) {
            /* names match - update rml info if provided */
            OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                 "%s daemon %s already known",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(name)));
            if (NULL != rml_uri) {
                if (NULL != proc->rml_uri) {
                    free(proc->rml_uri);
                }
                /* update in the rml too! */
                OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                     "%s updating daemon %s rml to %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_NAME_PRINT(name), rml_uri));
                if (ORTE_SUCCESS != (rc = orte_rml.set_contact_info(rml_uri))) {
                    ORTE_ERROR_LOG(rc);
                    return(rc);
                }
                proc->rml_uri = strdup(rml_uri);
            }
            return ORTE_SUCCESS;
        }
    }
    /* if we get here, then this daemon isn't already in
     * our data - add it
     */
    proc = OBJ_NEW(orte_proc_t);
    proc->name.jobid = name->jobid;
    proc->name.vpid = name->vpid;
    proc->node = node;
    proc->nodename = node->name;
    if (NULL != rml_uri) {
        proc->rml_uri = strdup(rml_uri);
    }
    /* update in the rml too! */
    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                         "%s updating daemon %s rml to %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(name), rml_uri));
    if (ORTE_SUCCESS != (rc = orte_rml.set_contact_info(rml_uri))) {
        ORTE_ERROR_LOG(rc);
        return(rc);
    }
    node->daemon = proc;
    OBJ_RETAIN(proc);
    /* add the daemon to the daemon job object */
    if (0 > (rc = opal_pointer_array_add(daemons->procs, (void*)proc))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(proc);
        return rc;
    }
    /* increment our num_procs */
    orte_process_info.num_procs++;
    
    /* and now we have to be careful - we need to ensure
     * that the -next- daemon we create doesn't overwrite
     * this vpid.
     *
     * NOTE: THIS METHOD WILL LEAVE GAPS IN THE DAEMON
     * VPIDS!! CAN ONLY BE USED WITH DIRECT ROUTED METHODS
     */
    if (name->vpid > daemons->num_procs) {
        daemons->num_procs = name->vpid + 1;
    }
    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                         "%s add daemon %s on node %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(name), nodename));
    return ORTE_SUCCESS;
}

static void ps_recv(int status,
                    orte_rmcast_channel_t channel,
                    orte_rmcast_tag_t tag,
                    orte_process_name_t *sender,
                    opal_buffer_t *buf, void *cbdata)
{
    opal_buffer_t response;
    orte_vpid_t vpid;
    int32_t rc;
    orte_odls_job_t *jobdat;
    orte_app_context_t *app;
    orte_odls_child_t *child;
    opal_list_item_t *item, *itm;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                         "%s ps cmd recvd",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    OBJ_CONSTRUCT(&response, opal_buffer_t);
    vpid = ORTE_VPID_INVALID;
    
    /* pack my nodename */
    opal_dss.pack(&response, &orte_process_info.nodename, 1, OPAL_STRING);
    
    /* loop through my local procs only */
    for (item = opal_list_get_first(&orte_local_jobdata);
         item != opal_list_get_end(&orte_local_jobdata);
         item = opal_list_get_next(item)) {
        jobdat = (orte_odls_job_t*)item;
        
        /* pack the name of the app for this job - we can have only one */
        app = jobdat->apps[0];
        opal_dss.pack(&response, &app->app, 1, OPAL_STRING);
        
        /* loop through the local children */
        for (itm = opal_list_get_first(&orte_local_children);
             itm != opal_list_get_end(&orte_local_children);
             itm = opal_list_get_next(itm)) {
                 child = (orte_odls_child_t*)itm;
  
            if (child->name->jobid != jobdat->jobid) {
                continue;
            }
            opal_dss.pack(&response, &(child->name->vpid), 1, ORTE_VPID);
        }
        /* flag the end for this job */
        opal_dss.pack(&response, &vpid, 1, ORTE_VPID);
    }

    if (ORTE_SUCCESS != (rc = orte_rmcast.send_buffer(ORTE_RMCAST_SYS_CHANNEL,
                                                      ORTE_RMCAST_TAG_PS,
                                                      &response))) {
        ORTE_ERROR_LOG(rc);
    }
    OBJ_DESTRUCT(&response);
}

static char* regen_uri(char *old_uri, orte_process_name_t *name)
{
    char *tmp, *new, *sname;
    int rc;
    
    tmp = strchr(old_uri, ';');
    tmp++;
    if (ORTE_SUCCESS != (rc = orte_util_convert_process_name_to_string(&sname, name))) {
        ORTE_ERROR_LOG(rc);
        return NULL;
    }
    asprintf(&new, "%s;%s", sname, tmp);
    free(sname);
    return new;
}

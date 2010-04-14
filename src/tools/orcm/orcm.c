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
#include "opal/class/opal_pointer_array.h"
#include "opal/event/event.h"
#include "opal/util/cmd_line.h"
#include "opal/util/argv.h"
#include "opal/util/opal_environ.h"
#include "opal/util/path.h"
#include "opal/mca/base/base.h"
#include "opal/mca/base/mca_base_param.h"
#include "opal/mca/sysinfo/base/base.h"
#include "opal/util/basename.h"
#include "opal/mca/installdirs/installdirs.h"

#include "orte/util/show_help.h"
#include "orte/util/parse_options.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/iof/iof.h"
#include "orte/mca/iof/base/base.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/odls/base/base.h"
#include "orte/mca/ras/ras.h"
#include "orte/mca/ras/base/base.h"
#include "orte/mca/rmaps/rmaps.h"
#include "orte/mca/rmaps/base/base.h"
#include "orte/mca/odls/odls.h"
#include "orte/runtime/orte_globals.h"

#include "mca/pnp/pnp.h"

/*****************************************
 * Global Vars for Command line Arguments
 *****************************************/
static struct {
    bool help;
    int verbosity;
    char *vm;
    bool do_not_launch;
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
    
    { "rmaps", "base", "no_schedule_local", '\0', "nolocal", "nolocal", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Do not schedule any applications on the local node" },

    { NULL, NULL, NULL, 'm', "dvm", "dvm", 1,
      &my_globals.vm, OPAL_CMD_LINE_TYPE_STRING,
      "ID of virtual machine to be used [number or file:name of file containing it" },
    
    { "orte", "default", "hostfile", 'h', "hostfile", "hostfile", 1,
      NULL, OPAL_CMD_LINE_TYPE_STRING,
      "Provide a default hostfile" },
   
    { "opal", "if", "do_not_resolve", '\0', "do-not-resolve", "do-not-resolve", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Do not attempt to resolve interfaces" },
    
    { "rmaps", "base", "display_map", '\0', "display-map", "display-map", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Display the process map just before launch"},
    { "rmaps", "base", "display_devel_map", '\0', "display-devel-map", "display-devel-map", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Display a detailed process map (mostly intended for developers) just before launch"},

    { "orte", "debug", NULL, 'd', "debug-devel", "debug-devel", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
      "Enable debugging of OpenRTE" },

    { NULL, NULL, NULL, '\0', "do-not-launch", "do-not-launch", 0,
      &my_globals.do_not_launch, OPAL_CMD_LINE_TYPE_BOOL,
     "Perform all necessary operations to prepare to launch the application, but do not actually launch it" },
    
    /* End of list */
    { NULL, NULL, NULL, '\0', NULL, NULL, 0,
      NULL, OPAL_CMD_LINE_TYPE_NULL,
      NULL }
};

/*
 * Local variables & functions
 */
static int num_apps = 0;
static orte_job_t *daemons;

static void tool_messages(int status,
                          orte_process_name_t *sender,
                          orcm_pnp_tag_t tag,
                          opal_buffer_t *buffer,
                          void *cbdata);

static void vm_commands(int status,
                        orte_process_name_t *sender,
                        orcm_pnp_tag_t tag,
                        opal_buffer_t *buffer,
                        void *cbdata);

static void spawn_app(char *cmd, bool add_procs, bool continuous, bool debug,
                      int restarts, int np, char *hosts, bool constrain);

static int kill_app(orte_job_t *jdata, char *replicas);

static void vm_tracker(char *app, char *version, char *release,
                       orte_process_name_t *name, char *node);

int main(int argc, char *argv[])
{
    int ret, i;
    opal_cmd_line_t cmd_line;
    char *cmd;
    int32_t vm, jfam;
    orte_node_t *node;
    orte_job_t *job;
    orte_process_name_t name;
    
    /***************
     * Initialize
     ***************/
    
    /* initialize the globals */
    my_globals.help = false;
    my_globals.verbosity = 5;
    my_globals.vm = NULL;
    my_globals.do_not_launch = false;
    
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
    
    /* check for bozo case */
    if (NULL == my_globals.vm) {
        fprintf(stderr, "Must specify ORCM distributed virtual machine to be used\n");
        return ORTE_ERROR;
    }

    if (0 == strncmp(my_globals.vm, "file", strlen("file")) ||
        0 == strncmp(my_globals.vm, "FILE", strlen("FILE"))) {
        char input[1024], *filename;
        FILE *fp;
        
        /* it is a file - get the filename */
        filename = strchr(my_globals.vm, ':');
        if (NULL == filename) {
            /* filename is not correctly formatted */
            orte_show_help("help-openrcm-runtime.txt", "hnp-filename-bad", true, "DVM", my_globals.vm);
            return ORTE_ERROR;
        }
        ++filename; /* space past the : */
        
        if (0 >= strlen(filename)) {
            /* they forgot to give us the name! */
            orte_show_help("help-openrcm-runtime.txt", "hnp-filename-bad", true, "virtual machine", my_globals.vm);
            return ORTE_ERROR;
        }
        
        /* open the file and extract the pid */
        fp = fopen(filename, "r");
        if (NULL == fp) { /* can't find or read file! */
            orte_show_help("help-openrcm-runtime.txt", "hnp-filename-access", true, "virtual machine", filename);
            return ORTE_ERROR;
        }
        if (NULL == fgets(input, 1024, fp)) {
            /* something malformed about file */
            fclose(fp);
            orte_show_help("help-openrcm-runtime.txt", "hnp-file-bad", "virtual machine", true, filename);
            return ORTE_ERROR;
        }
        fclose(fp);
        input[strlen(input)-1] = '\0';  /* remove newline */
        /* convert the id */
        vm = strtoul(input, NULL, 10);
    } else {
        /* should just be the master itself */
        vm = strtoul(my_globals.vm, NULL, 10);
    }

    /* open a debug channel and set the verbosity */
    orcm_debug_output = opal_output_open(NULL);
    opal_output_set_verbosity(orcm_debug_output, my_globals.verbosity);
    

    /***************************
     * Init as an ORCM_TOOL so we properly connect to an existing VM
     * We are also an IOF endpt
     ***************************/
    if (ORTE_SUCCESS != orcm_init(ORCM_TOOL | ORCM_IOF_ENDPT)) {
        orcm_finalize();
        return 1;
    }

    /* there are some additional frameworks we need */
    if (ORTE_SUCCESS != (ret = opal_sysinfo_base_open())) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    if (ORTE_SUCCESS != (ret = opal_sysinfo_base_select())) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }

    if (ORTE_SUCCESS != (ret = orte_ras_base_open())) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    if (ORTE_SUCCESS != (ret = orte_ras_base_select())) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    if (ORTE_SUCCESS != (ret = orte_rmaps_base_open())) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    if (ORTE_SUCCESS != (ret = orte_rmaps_base_select())) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    if (ORTE_SUCCESS != (ret = orte_odls_base_open())) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    if (ORTE_SUCCESS != (ret = orte_odls_base_select())) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    if (ORTE_SUCCESS != (ret = orte_iof_base_open())) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    if (ORTE_SUCCESS != (ret = orte_iof_base_select())) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    /* do a "push" on the iof to ensure the recv gets issued - doesn't
     * matter what values we supply
     */
    if (ORTE_SUCCESS != (ret = orte_iof.push(NULL, ORTE_IOF_STDOUT, -1))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    /* setup the global job and node arrays */
    orte_job_data = OBJ_NEW(opal_pointer_array_t);
    if (ORTE_SUCCESS != (ret = opal_pointer_array_init(orte_job_data,
                                                       1,
                                                       ORTE_GLOBAL_ARRAY_MAX_SIZE,
                                                       1))) {
        ORTE_ERROR_LOG(ret);
        goto xtra_cleanup;
    }
    
    orte_node_pool = OBJ_NEW(opal_pointer_array_t);
    if (ORTE_SUCCESS != (ret = opal_pointer_array_init(orte_node_pool,
                                                       ORTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                                       ORTE_GLOBAL_ARRAY_MAX_SIZE,
                                                       ORTE_GLOBAL_ARRAY_BLOCK_SIZE))) {
        ORTE_ERROR_LOG(ret);
        goto xtra_cleanup;
    }
 
    /* Setup the job data object for the daemons */        
    daemons = OBJ_NEW(orte_job_t);
    /* the daemons are in the VM job */
    jfam = ORTE_CONSTRUCT_JOB_FAMILY(vm);
    daemons->jobid = ORTE_CONSTRUCT_LOCAL_JOBID(jfam, 0);
    daemons->state = ORTE_JOB_STATE_RUNNING;
    opal_pointer_array_set_item(orte_job_data, 0, daemons);
    
    /* add our node to the pool - may not be used depending upon
     * mapping options, but needs to be there to avoid segfaults
     * when no other nodes are available
     */
    node = OBJ_NEW(orte_node_t);
    node->name = strdup(orte_process_info.nodename);
    node->state = ORTE_NODE_STATE_UP;
    node->index = opal_pointer_array_add(orte_node_pool, node);
    
    /* set our mapping policy to use the VM to get nodes */
    ORTE_SET_MAPPING_POLICY(ORTE_MAPPING_USE_VM);
    /* use byslot mapping by default */
    ORTE_ADD_MAPPING_POLICY(ORTE_MAPPING_BYSLOT);
    
    /* register to catch launch requests */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_input_buffer("orcm-start", "0.1", "alpha",
                                                              ORCM_PNP_SYS_CHANNEL,
                                                              ORCM_PNP_TAG_TOOL,
                                                              tool_messages))) {
        ORTE_ERROR_LOG(ret);
        goto xtra_cleanup;
    }
    
    /* register to catch stop requests */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_input_buffer("orcm-stop", "0.1", "alpha",
                                                              ORCM_PNP_SYS_CHANNEL,
                                                              ORCM_PNP_TAG_TOOL,
                                                              tool_messages))) {
        ORTE_ERROR_LOG(ret);
        goto xtra_cleanup;
    }
    
    /* listen for DVM commands */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_input_buffer("orcm-vm", "0.1", "alpha",
                                                              ORCM_PNP_SYS_CHANNEL,
                                                              ORCM_PNP_TAG_COMMAND,
                                                              vm_commands))) {
        ORTE_ERROR_LOG(ret);
        goto xtra_cleanup;
    }
    
    /* announce my existence */
    if (ORCM_SUCCESS != (ret = orcm_pnp.announce("ORCM", "0.1", "alpha", vm_tracker))) {
        ORTE_ERROR_LOG(ret);
        goto xtra_cleanup;
    }
    
    opal_output(orte_clean_output, "\nORCM %s NOW RUNNING...ATTACHED TO DVM %s\n",
                ORTE_JOB_FAMILY_PRINT(ORTE_PROC_MY_NAME->jobid),
                ORTE_JOB_FAMILY_PRINT(daemons->jobid));

    /* just wait until the abort is fired */
    opal_event_dispatch();

    /***************
     * Cleanup
     ***************/
xtra_cleanup:
    /* close the extra frameworks */
    orte_iof_base_close();
    orte_ras_base_close();
    orte_rmaps_base_close();
    orte_odls_base_close();
    opal_sysinfo_base_close();
    
cleanup:
    ORTE_UPDATE_EXIT_STATUS(orte_exit_status);
    
    /* Remove the signal handlers */
    orcm_remove_signal_handlers();
    
    /* cleanup the job and node info arrays */
    if (NULL != orte_node_pool) {
        for (i=0; i < orte_node_pool->size; i++) {
            if (NULL != (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool,i))) {
                OBJ_RELEASE(node);
            }
        }
        OBJ_RELEASE(orte_node_pool);
    }
    if (NULL != orte_job_data) {
        for (i=0; i < orte_job_data->size; i++) {
            if (NULL != (job = (orte_job_t*)opal_pointer_array_get_item(orte_job_data,i))) {
                OBJ_RELEASE(job);
            }
        }
        OBJ_RELEASE(orte_job_data);
    }

    /* whack any lingering session directory files from our jobs */
    orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
    
    /* cleanup and leave */
    orcm_finalize();
    
    exit(orte_exit_status);
    return orte_exit_status;
}

static void tool_messages(int status,
                          orte_process_name_t *sender,
                          orcm_pnp_tag_t tag,
                          opal_buffer_t *buffer,
                          void *cbdata)
{
    char *cmd, *hosts;
    int32_t rc=ORCM_SUCCESS, n, j, num_apps, restarts;
    opal_buffer_t response;
    orte_job_t *jdata;
    orte_app_context_t *app;
    orte_proc_t *proc;
    orte_vpid_t vpid;
    orcm_tool_cmd_t flag;
    int8_t constrain, add_procs, debug, continuous;
    char *replicas;
    int32_t ljob;
    uint16_t jfam;

    /* if this isn't intended for me or for the DVM I am scheduling, ignore it */
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &jfam, &n, OPAL_UINT16))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    if (jfam != ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid)) {
        opal_output(0, "%s NOT FOR ME!", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        return;
    }
    
    /* unpack the cmd */
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &flag, &n, ORCM_TOOL_CMD_T))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* setup the response */
    OBJ_CONSTRUCT(&response, opal_buffer_t);
    /* pack the job family of the sender so they know it is meant for them */
    jfam  = ORTE_JOB_FAMILY(sender->jobid);
    opal_dss.pack(&response, &jfam, 1, OPAL_UINT16);
    /* return the cmd flag */
    opal_dss.pack(&response, &flag, 1, ORCM_TOOL_CMD_T);
    
    if (ORCM_TOOL_START_CMD == flag) {
        OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                             "%s spawn cmd from %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(sender)));
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
        /* unpack the continuous flag */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &continuous, &n, OPAL_INT8))) {
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
        spawn_app(cmd, add_procs, continuous, debug, restarts, num_apps, hosts, constrain);
    } else if (ORCM_TOOL_STOP_CMD == flag) {
        n=1;
        while (ORTE_SUCCESS == (rc = opal_dss.unpack(buffer, &cmd, &n, OPAL_STRING))) {
            OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                                 "%s kill cmd from %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(sender)));
            /* unpack the replica info */
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &replicas, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                goto cleanup;
            }
            /* find all job data objects for this app - skip the daemon job
             * We have to check all the jobs because there could be multiple
             * invocations of the same application
             */
            for (n=1; n < orte_job_data->size; n++) {
                if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, n))) {
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
                    return;
                }
                if (0 == strcasecmp(cmd, app->app)) {
                    if (ORTE_SUCCESS != (rc = kill_app(jdata, replicas))) {
                        ORTE_ERROR_LOG(rc);
                    }
                }
            }
            free(replicas);
            n=1;
        }
        if (ORTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            ORTE_ERROR_LOG(rc);
        } else {
            rc = ORTE_SUCCESS;
        }
    } else {
        opal_output(0, "%s: UNKNOWN TOOL CMD FLAG %d", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)flag);
    }
    
cleanup:
    opal_output(0, "sending response to %s", ORTE_NAME_PRINT(sender));
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_buffer(ORCM_PNP_SYS_CHANNEL,
                                                     NULL, ORCM_PNP_TAG_TOOL,
                                                     &response))) {
        ORTE_ERROR_LOG(rc);
    }
    OBJ_DESTRUCT(&response);
}

static void spawn_app(char *cmd, bool add_procs, bool continuous, bool debug,
                      int restarts, int np, char *hosts, bool constrain)
{
    int rc, i, n;
    orte_job_t *jdata;
    orte_proc_t *proc;
    orte_app_context_t *app;
    char *param, *value;
    char cwd[OPAL_PATH_MAX];
    char **inpt;
    orte_daemon_cmd_flag_t command;
    opal_buffer_t buffer;
    int32_t ljob;
    uint16_t jfam;

    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                         "%s spawn:app: %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), cmd));
    
    /* if we are adding procs, find the existing job object */
    if (add_procs) {
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
    
    /* create a new job for this app */
    jdata = OBJ_NEW(orte_job_t);
    if (ORTE_SUCCESS != (rc = orte_plm_base_create_jobid(jdata))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(jdata);
        return;
    }
    /* store it on the global job data pool */
    ljob = ORTE_LOCAL_JOBID(jdata->jobid);
    opal_pointer_array_set_item(orte_job_data, ljob, jdata);
    
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
    
    /* run the allocator */
    if (ORTE_SUCCESS != (rc = orte_ras.allocate(jdata))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(jdata);
        return;
    }
    
    /* indicate that this is to be a continuously operating job - i.e.,
     * we restart processes even if they normally terminate
     */
    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                         "%s setting controls to %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (continuous) ? "CONTINUOUS" : "NON-CONTINUOUS"));
    if (continuous) {
        jdata->controls |= ORTE_JOB_CONTROL_CONTINUOUS_OP;
    }
    /* we don't forward stdin */
    jdata->stdin_target = ORTE_VPID_INVALID;

    /* pass max number of restarts */
    jdata->max_restarts = restarts;
    
launch:
    /* if we want to debug the apps, set the proper envar */
    if (debug) {
        opal_setenv("ORCM_MCA_spin", "1", true, &app->env);
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_debug_output,
                         "%s LAUNCHING APP %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), app->app));
    
    /* map it */
    if (ORTE_SUCCESS != (rc = orte_rmaps.map_job(jdata))) {
        ORTE_ERROR_LOG(rc);
        return;
    }         
    
    /* if we don't want to launch, then we are done */
    if (my_globals.do_not_launch) {
        return;
    }
    
    /* setup the buffer */
    OBJ_CONSTRUCT(&buffer, opal_buffer_t);
    
    /* include the job family of the target dvm */
    jfam  = ORTE_JOB_FAMILY(daemons->jobid);
    opal_dss.pack(&buffer, &jfam, 1, OPAL_UINT16);

    /* pack the add_local_procs command */
    command = ORTE_DAEMON_ADD_LOCAL_PROCS;
    if (ORTE_SUCCESS != (rc = opal_dss.pack(&buffer, &command, 1, ORTE_DAEMON_CMD))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&buffer);
        return;
    }
    
    /* get the local launcher's required data */
    if (ORTE_SUCCESS != (rc = orte_odls.get_add_procs_data(&buffer, jdata->jobid))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&buffer);
        return;
    }
    
    /* send it to the vm */
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_buffer(ORCM_PNP_SYS_CHANNEL,
                                                     NULL, ORCM_PNP_TAG_COMMAND,
                                                     &buffer))) {
        ORTE_ERROR_LOG(rc);
    }
    OBJ_DESTRUCT(&buffer);
    return;
}

static int kill_app(orte_job_t *jdata, char *replicas)
{   
    int rc;
    opal_buffer_t cmd;
    orte_daemon_cmd_flag_t command=ORTE_DAEMON_KILL_LOCAL_PROCS;
    int i, v;
    char **vpids=NULL;
    orte_proc_t *proc;
    int32_t num_replicas;
    uint16_t jfam;
    
    OBJ_CONSTRUCT(&cmd, opal_buffer_t);

    /* include the job family of the target dvm */
    jfam  = ORTE_JOB_FAMILY(daemons->jobid);
    opal_dss.pack(&cmd, &jfam, 1, OPAL_UINT16);

    if (NULL != replicas) {
        orte_util_parse_range_options(replicas, &vpids);
    } else {
        opal_argv_append_nosize(&vpids, "-1");
    }
    num_replicas = opal_argv_count(vpids);

    /* pack the command */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(&cmd, &command, 1, ORTE_DAEMON_CMD))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&cmd);
        opal_argv_free(vpids);
        return rc;
    }
    
    /* pack the number of procs */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(&cmd, &num_replicas, 1, OPAL_INT32))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&cmd);
        opal_argv_free(vpids);
        return rc;
    }
    
    /* pack the proc names */
    for (i=0; NULL != vpids[i]; i++) {
        v = strtol(vpids[i], NULL, 10);
        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, v))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            continue;
        }
        if (ORTE_SUCCESS != (rc = opal_dss.pack(&cmd, &(proc->name), 1, ORTE_NAME))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&cmd);
            opal_argv_free(vpids);
            return rc;
        }
    }
    opal_argv_free(vpids);
    
    /* send it to the vm */
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_buffer(ORCM_PNP_SYS_CHANNEL,
                                                     NULL, ORCM_PNP_TAG_COMMAND,
                                                     &cmd))) {
        ORTE_ERROR_LOG(rc);
    }
    OBJ_DESTRUCT(&cmd);
    
    return rc;
}

static void vm_tracker(char *app, char *version, char *release,
                       orte_process_name_t *name, char *nodename)
{
    orte_proc_t *proc;
    orte_node_t *node;
    int i;
    
    /* if this isn't from my VM, ignore it */
    if (ORTE_JOB_FAMILY(name->jobid) != ORTE_JOB_FAMILY(daemons->jobid)) {
        return;
    }
    
    /* if it is a daemon, record it */
    if (ORTE_JOBID_IS_DAEMON(name->jobid)) {
        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(daemons->procs, name->vpid))) {
            /* new daemon - add it */
            proc = OBJ_NEW(orte_proc_t);
            proc->name.jobid = name->jobid;
            proc->name.vpid = name->vpid;
            daemons->num_procs++;
            opal_pointer_array_add(daemons->procs, proc);
        }
        /* ensure the state is set to running */
        proc->state = ORTE_PROC_STATE_RUNNING;
        /* if it is a restart, check the node against the
         * new one to see if it changed location
         */
        if (NULL != proc->nodename) {
            if (0 == strcmp(nodename, proc->nodename)) {
                /* all done */
                return;
            }
            /* must have moved */
            OBJ_RELEASE(proc->node);  /* maintain accounting */
            proc->nodename = NULL;
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
    }
}

static void vm_commands(int status,
                        orte_process_name_t *sender,
                        orcm_pnp_tag_t tag,
                        opal_buffer_t *buffer,
                        void *cbdata)
{
    int rc, n, i;
    orte_daemon_cmd_flag_t command;
    uint16_t jfam;
    orte_job_t *job;
    orte_node_t *node;
    
    
    /* if this isn't intended for my DVM, ignore it */
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &jfam, &n, OPAL_UINT16))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    if (jfam != ORTE_JOB_FAMILY(daemons->jobid)) {
        opal_output(0, "%s GOT COMMAND FOR DVM %d - NOT FOR MINE(%s)!",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), jfam,
                    ORTE_JOB_FAMILY_PRINT(daemons->jobid));
        return;
    }
    
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &command, &n, ORTE_DAEMON_CMD))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    if (ORTE_DAEMON_HALT_VM_CMD == command) {
        /* Remove the signal handlers */
        orcm_remove_signal_handlers();
        
        /* cleanup the job and node info arrays */
        if (NULL != orte_node_pool) {
            for (i=0; i < orte_node_pool->size; i++) {
                if (NULL != (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool,i))) {
                    OBJ_RELEASE(node);
                }
            }
            OBJ_RELEASE(orte_node_pool);
        }
        if (NULL != orte_job_data) {
            for (i=0; i < orte_job_data->size; i++) {
                if (NULL != (job = (orte_job_t*)opal_pointer_array_get_item(orte_job_data,i))) {
                    OBJ_RELEASE(job);
                }
            }
            OBJ_RELEASE(orte_job_data);
        }
        
        /* whack any lingering session directory files from our jobs */
        orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
        
        /* cleanup and leave */
        orcm_finalize();
        
        exit(orte_exit_status);
    }        
}

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

    { "orte", "do_not", "launch", '\0', "do-not-launch", "do-not-launch", 0,
      NULL, OPAL_CMD_LINE_TYPE_BOOL,
     "Perform all necessary operations to prepare to launch the application, but do not actually launch it" },
    
    /* End of list */
    { NULL, NULL, NULL, '\0', NULL, NULL, 0,
      NULL, OPAL_CMD_LINE_TYPE_NULL,
      NULL }
};

/*
 * Local variables & functions
 */
static orte_job_t *daemons;

static void vm_commands(int status,
                        orte_process_name_t *sender,
                        orcm_pnp_tag_t tag,
                        opal_buffer_t *buffer,
                        void *cbdata);

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
    my_globals.verbosity = 0;
    my_globals.vm = NULL;
    
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
        fprintf(stderr, "Must specify ORCM DVM to be used\n");
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
    
    if (ORTE_SUCCESS != (ret = orcm_cfgi_base_open())) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    if (ORTE_SUCCESS != (ret = orcm_cfgi_base_select())) {
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
    orcm_cfgi_base_close();
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

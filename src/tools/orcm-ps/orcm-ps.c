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
#include "src/runtime/runtime.h"
#include "include/constants.h"

#include "orte_config.h"
#include "orte/constants.h"

#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif  /*  HAVE_STDLIB_H */
#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif  /*  HAVE_SIGNAL_H */
#ifdef HAVE_STRING_H
#include <string.h>
#endif  /* HAVE_STRING_H */

#include "opal/mca/event/event.h"
#include "opal/util/cmd_line.h"
#include "opal/util/argv.h"
#include "opal/runtime/opal.h"
#include "opal/util/opal_environ.h"
#include "opal/threads/threads.h"

#include "orte/util/show_help.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"

#include "mca/pnp/pnp.h"

/*****************************************
 * Global Vars for Command line Arguments
 *****************************************/
static struct {
    bool help;
    bool monitor;
    int update_rate;
    char *sched;
    char *hnp_uri;
    bool all;
} my_globals;

opal_cmd_line_init_t cmd_line_opts[] = {
    { NULL, NULL, NULL, 'h', NULL, "help", 0,
      &my_globals.help, OPAL_CMD_LINE_TYPE_BOOL,
      "Print help message" },

    { NULL, NULL, NULL, 'm', "monitor", "monitor", 0,
      &my_globals.monitor, OPAL_CMD_LINE_TYPE_BOOL,
      "Provide a regularly updated picture of the system (default: single snapshot)" },
    
    { NULL, NULL, NULL, '\0', "update-rate", "update-rate", 1,
      &my_globals.update_rate, OPAL_CMD_LINE_TYPE_INT,
      "Update rate in seconds (default: 5)" },
    
    { NULL, NULL, NULL, 'd', "dvm", "dvm", 1,
      &my_globals.sched, OPAL_CMD_LINE_TYPE_STRING,
      "ORCM DVM to be contacted [number or file:name of file containing it" },
    
    { NULL, NULL, NULL, '\0', "uri", "uri", 1,
      &my_globals.hnp_uri, OPAL_CMD_LINE_TYPE_STRING,
      "The uri of the ORCM DVM [uri or file:name of file containing it" },
    
    { NULL, NULL, NULL, 'a', "all", "all", 0,
      &my_globals.all, OPAL_CMD_LINE_TYPE_BOOL,
      "Show info from all source replicas [default: only from leader]" },
    
    /* End of list */
    { NULL, NULL, NULL, '\0', NULL, NULL, 0,
      NULL, OPAL_CMD_LINE_TYPE_NULL,
      NULL }
};

/* Local object for tracking responses */

/*
 * Local variables & functions
 */
static opal_mutex_t lock;
static opal_condition_t cond;
static bool waiting=true;
static orte_jobid_t jobfam;

static void pretty_print(opal_buffer_t *buf);

static void ps_recv(int status,
                    orte_process_name_t *sender,
                    orcm_pnp_tag_t tag,
                    struct iovec *msg, int count,
                    opal_buffer_t *buf, void *cbdata);

static void vm_tracker(char *app, char *version, char *release,
                       orte_process_name_t *name, char *nodename,
                       char *rml_uri, uint32_t uid);

/* update data function */
static void update_data(int fd, short flg, void *arg)
{
    opal_buffer_t buf;
    opal_event_t *tmp;
    struct timeval now;
    int32_t ret;
    time_t mytime;
    orte_process_name_t name;

    /* indicate the dvm */
    name.jobid = jobfam;
    if (my_globals.all) {
        /* if the user asked for -all- info, then we indicate
         * that all members of the dvm are to respond
         */
        name.vpid = ORTE_VPID_WILDCARD;
    } else {
        /* indicate that only the lead member is to respond */
        name.vpid = ORTE_VPID_INVALID;
    }

    if (NULL != arg) {
        /* print a separator for the next output */
        fprintf(stderr, "\n=========================================================\n");
        time(&mytime);
        fprintf(stderr, "Time: %s\n", ctime(&mytime));
    }

    /* setup the buffer to send our cmd */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    
    if (ORTE_SUCCESS != (ret = opal_dss.pack(&buf, &name, 1, ORTE_NAME))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    if (ORCM_SUCCESS != (ret = orcm_pnp.output(ORCM_PNP_SYS_CHANNEL,
                                               NULL, ORCM_PNP_TAG_PS,
                                               NULL, 0, &buf))) {
        ORTE_ERROR_LOG(ret);
    }
    
    /* now wait for response */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &waiting);

cleanup:
    OBJ_DESTRUCT(&buf);
    if (NULL != arg) {
        /* reset the timer */
        tmp = (opal_event_t*)arg;
        now.tv_sec = my_globals.update_rate;
        now.tv_usec = 0;
        opal_event_evtimer_add(tmp, &now);
    }
}

int main(int argc, char *argv[])
{
    int32_t ret;
    opal_cmd_line_t cmd_line;
    char *args = NULL;
    
    /***************
     * Initialize
     ***************/

    /*
     * Make sure to init util before parse_args
     * to ensure installdirs is setup properly
     * before calling mca_base_open();
     */
    if( ORTE_SUCCESS != (ret = orcm_init_util()) ) {
        return ret;
    }
    
    /* initialize the globals */
    my_globals.help = false;
    my_globals.monitor = false;
    my_globals.update_rate = 5;
    my_globals.sched = NULL;
    my_globals.hnp_uri = NULL;
    my_globals.all = false;
    
    /* Parse the command line options */
    opal_cmd_line_create(&cmd_line, cmd_line_opts);
    
    mca_base_open();
    mca_base_cmd_line_setup(&cmd_line);
    ret = opal_cmd_line_parse(&cmd_line, true, argc, argv);
    
    /* extract the MCA/GMCA params */
    mca_base_cmd_line_process_args(&cmd_line, &environ, &environ);

    /**
     * Now start parsing our specific arguments
     */
    if (OPAL_SUCCESS != ret || my_globals.help) {
        args = opal_cmd_line_get_usage_msg(&cmd_line);
        orte_show_help("help-orcm-ps.txt", "usage", true, args);
        free(args);
        return ORTE_ERROR;
    }
    
    /* need to specify scheduler */
    if (NULL == my_globals.sched && NULL == my_globals.hnp_uri) {
        opal_output(0, "Must specify ORCM DVM to be contacted");
        return ORTE_ERROR;
    }

    if (NULL != my_globals.sched) {
        if (0 == strncmp(my_globals.sched, "file", strlen("file")) ||
            0 == strncmp(my_globals.sched, "FILE", strlen("FILE"))) {
            char input[1024], *filename;
            FILE *fp;
            
            /* it is a file - get the filename */
            filename = strchr(my_globals.sched, ':');
            if (NULL == filename) {
                /* filename is not correctly formatted */
                orte_show_help("help-openrcm-runtime.txt", "hnp-filename-bad", true, "scheduler", my_globals.sched);
                return ORTE_ERROR;
            }
            ++filename; /* space past the : */
            
            if (0 >= strlen(filename)) {
                /* they forgot to give us the name! */
                orte_show_help("help-openrcm-runtime.txt", "hnp-filename-bad", true, "scheduler", my_globals.sched);
                return ORTE_ERROR;
            }
            
            /* open the file and extract the pid */
            fp = fopen(filename, "r");
            if (NULL == fp) { /* can't find or read file! */
                orte_show_help("help-openrcm-runtime.txt", "hnp-filename-access", true, "scheduler", filename);
                return ORTE_ERROR;
            }
            if (NULL == fgets(input, 1024, fp)) {
                /* something malformed about file */
                fclose(fp);
                orte_show_help("help-openrcm-runtime.txt", "hnp-file-bad", "scheduler", true, filename);
                return ORTE_ERROR;
            }
            fclose(fp);
            input[strlen(input)-1] = '\0';  /* remove newline */
            /* get the hnp uri and convert it */
        } else {
            /* must just be the hnp uri - convert it */
        }
    }
            
    OBJ_CONSTRUCT(&lock, opal_mutex_t);
    OBJ_CONSTRUCT(&cond, opal_condition_t);
    
    /***************************
     * We need all of OPAL and ORTE - this will
     * automatically connect us to the CM
     ***************************/
    if (ORCM_SUCCESS != (ret = orcm_init(ORCM_TOOL))) {
        goto cleanup;
    }

    /* register to receive responses */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcm-ps", "0.1", "alpha",
                                                         ORCM_PNP_GROUP_INPUT_CHANNEL,
                                                         ORCM_PNP_TAG_PS,
                                                         ps_recv, NULL))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    /* announce my existence */
    if (ORCM_SUCCESS != (ret = orcm_pnp.announce("orcm-ps", "0.1", "alpha", vm_tracker))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    /* we know we need to print the data once */
    update_data(0, 0, NULL);
    
    if (my_globals.monitor) {
        /* setup a timer event to tell us when to do it next */
        ORTE_TIMER_EVENT(my_globals.update_rate, 0, update_data);
    } else {
        ret = 0;
        goto cleanup;
    }
    
    opal_event_dispatch(opal_event_base);
    
    /***************
     * Cleanup
     ***************/
 cleanup:
    OBJ_DESTRUCT(&lock);
    OBJ_DESTRUCT(&cond);
    
    /* cleanup orcm */
    orcm_finalize();

    return ret;
}

static void pretty_print(opal_buffer_t *buf)
{
    char *app;
    int32_t n, j;
    orte_vpid_t vpid;
    char *node;
    int rc;
    
    n=1;
    while (ORTE_SUCCESS == (rc = opal_dss.unpack(buf, &app, &n, OPAL_STRING))) {
        fprintf(stderr, "APP: %s\n", app);
        free(app);
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &vpid, &n, ORTE_VPID))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        while (ORTE_VPID_INVALID != vpid) {
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &node, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            fprintf(stderr, "    %s: %s\n", ORTE_VPID_PRINT(vpid), node);
            free(node);
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &vpid, &n, ORTE_VPID))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
        }
        fprintf(stderr, "\n");
    }
    if (ORTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        ORTE_ERROR_LOG(rc);
    } }

static void ps_recv(int status,
                    orte_process_name_t *sender,
                    orcm_pnp_tag_t tag,
                    struct iovec *msg, int count,
                    opal_buffer_t *buf, void *cbdata)
{
    orcm_tool_cmd_t flag;
    char *app;
    int32_t n, j;
    orte_vpid_t vpid;
    char *node;
    int rc;

    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &node, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &node, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    fprintf(stderr, "NODE: %s\n", node);
    free(node);
    n=1;
    while (ORTE_SUCCESS == (rc = opal_dss.unpack(buf, &app, &n, OPAL_STRING))) {
        fprintf(stderr, "    APP: %s\n", app);
        free(app);
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &vpid, &n, ORTE_VPID))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        fprintf(stderr, "      Instances: ");
        while (ORTE_VPID_INVALID != vpid) {
            fprintf(stderr, "%s ", ORTE_VPID_PRINT(vpid));
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &vpid, &n, ORTE_VPID))) {
                ORTE_ERROR_LOG(rc);
                goto cleanup;
            }
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");

cleanup:
    /* release the wait */
    OPAL_RELEASE_THREAD(&lock, &cond, &waiting);
}

static void vm_tracker(char *app, char *version, char *release,
                       orte_process_name_t *name, char *nodename,
                       char *rml_uri, uint32_t uid)
{
    orte_proc_t *proc;
    orte_node_t *node;
    orte_job_t *daemons;
    int i;
    
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


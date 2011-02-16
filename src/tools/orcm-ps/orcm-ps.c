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
#include "opal/util/basename.h"
#include "opal/util/fd.h"

#include "orte/threads/threads.h"
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
    int sched;
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
      &my_globals.sched, OPAL_CMD_LINE_TYPE_INT,
      "ORCM DVM to be contacted [default: 0]" },
    
    /* End of list */
    { NULL, NULL, NULL, '\0', NULL, NULL, 0,
      NULL, OPAL_CMD_LINE_TYPE_NULL,
      NULL }
};

/* Local object for tracking responses */

/*
 * Local variables & functions
 */
static int rel_pipe[2];
static opal_event_t rel_ev;

static void ps_recv(int status,
                    orte_process_name_t *sender,
                    orcm_pnp_tag_t tag,
                    struct iovec *msg, int count,
                    opal_buffer_t *buf, void *cbdata);

static void cbfunc(int status,
                   orte_process_name_t *sender,
                   orcm_pnp_tag_t tag,
                   struct iovec *msg,
                   int count,
                   opal_buffer_t *buffer,
                   void *cbdata)
{
    OBJ_RELEASE(buffer);
}

static void process_release(int fd, short flag, void *data)
{
    /* delete our local event */
    opal_event_del(&rel_ev);
    close(rel_pipe[0]);
    close(rel_pipe[1]);
    orcm_finalize();
    exit(0);
}

/* update data function */
static void update_data(int fd, short flg, void *arg)
{
    opal_buffer_t *buf;
    int32_t ret;
    orte_process_name_t name;

    /* setup the buffer to send our cmd */
    buf = OBJ_NEW(opal_buffer_t);
    
    name.jobid = ORTE_CONSTRUCT_LOCAL_JOBID(my_globals.sched, 0);
    name.vpid = 0;
    if (ORTE_SUCCESS != (ret = opal_dss.pack(buf, &name, 1, ORTE_NAME))) {
        ORTE_ERROR_LOG(ret);
        return;
    }
    
    if (ORCM_SUCCESS != (ret = orcm_pnp.output_nb(ORCM_PNP_SYS_CHANNEL,
                                               NULL, ORCM_PNP_TAG_PS,
                                                  NULL, 0, buf, cbfunc, NULL))) {
        ORTE_ERROR_LOG(ret);
        OBJ_RELEASE(buf);
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
    my_globals.sched = 0;
    
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
    if (ORCM_SUCCESS != (ret = orcm_pnp.announce("orcm-ps", "0.1", "alpha", NULL))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    /* define an event to signal completion */
    if (pipe(rel_pipe) < 0) {
        opal_output(0, "Cannot open release pipe");
        goto cleanup;
    }
    opal_event_set(opal_event_base, &rel_ev, rel_pipe[0],
                   OPAL_EV_READ, process_release, NULL);
    opal_event_add(&rel_ev, 0);

    /* we know we need to print the data once */
    update_data(0, 0, NULL);
    
    opal_event_dispatch(opal_event_base);
    
    /***************
     * Cleanup
     ***************/
 cleanup:    
    /* cleanup orcm */
    orcm_finalize();

    return ret;
}

static void ps_recv(int status,
                    orte_process_name_t *sender,
                    orcm_pnp_tag_t tag,
                    struct iovec *msg, int count,
                    opal_buffer_t *buf, void *cbdata)
{
    orte_jobid_t jobid;
    char *jname=NULL;
    orte_app_idx_t idx;
    char *app=NULL;
    int32_t n;
    int32_t max_restarts;
    orte_vpid_t vpid;
    pid_t pid;
    char *node=NULL;
    int32_t restarts;
    bool jfirst, afirst;
    int rc;

    /* output the title bars */
    opal_output(orte_clean_output, "JOB\t    JOB    \t  APP  \t        \t MAX");
    opal_output(orte_clean_output, "ID \t    NAME   \tCONTEXT\t  PATH  \tRESTARTS\tVPID\t  PID \t  NODE  \tRESTARTS");

    n=1;
    while (ORTE_SUCCESS == (rc = opal_dss.unpack(buf, &jobid, &n, ORTE_JOBID))) {
        /* get the job name */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &jname, &n, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            goto release;
        }

        n=1;
        jfirst = true;
        afirst = true;
        while (ORTE_SUCCESS == (rc = opal_dss.unpack(buf, &idx, &n, ORTE_APP_IDX)) &&
               idx != ORTE_APP_IDX_MAX) {
            /* get the app data */
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &app, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                goto release;
            }
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &max_restarts, &n, OPAL_INT32))) {
                ORTE_ERROR_LOG(rc);
                goto release;
            }

            /* unload the procs */
            n=1;
            while (ORTE_SUCCESS == (rc = opal_dss.unpack(buf, &vpid, &n, ORTE_VPID)) &&
                   vpid != ORTE_VPID_INVALID) {
                n=1;
                if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &pid, &n, OPAL_PID))) {
                    ORTE_ERROR_LOG(rc);
                    goto release;
                }
                n=1;
                if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &node, &n, OPAL_STRING))) {
                    ORTE_ERROR_LOG(rc);
                    goto release;
                }
                n=1;
                if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &restarts, &n, OPAL_INT32))) {
                    ORTE_ERROR_LOG(rc);
                    goto release;
                }
                if (jfirst) {
                    /* output the job and app data with the proc */
                    opal_output(orte_clean_output, "%d\t%8s\t  %d\t%8s\t%8d\t%4s\t%6d\t%8s\t%d",
                                (int)ORTE_LOCAL_JOBID(jobid), jname, idx,
                                opal_basename(app), max_restarts,
                                ORTE_VPID_PRINT(vpid), (int)pid, node, restarts);
                    /* flag that we did the first one */
                    jfirst = false;
                } else if (afirst) {
                    opal_output(orte_clean_output, "\t\t\t  %d\t%8s\t%8d\t%4s\t%6d\t%8s\t%d",
                                idx, opal_basename(app), max_restarts,
                                ORTE_VPID_PRINT(vpid), (int)pid, node, restarts);
                    afirst = false;
                } else {
                    opal_output(orte_clean_output, "\t\t    \t    \t   \t%4s\t%6d\t%8s\t%d",
                                ORTE_VPID_PRINT(vpid), (int)pid, node, restarts);
                }
                free(node);
                node = NULL;
            }
            n=1;
            free(app);
            app = NULL;
        }
        n=1;
        free(jname);
        jname = NULL;
    }

 release:
    if (NULL != jname) {
        free(jname);
    }
    if (NULL != app) {
        free(app);
    }
    if (NULL != node) {
        free(node);
    }

    opal_fd_write(rel_pipe[1], sizeof(int), &rc);
}

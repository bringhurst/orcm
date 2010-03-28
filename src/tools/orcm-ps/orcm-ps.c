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

#include "opal/event/event.h"
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
    char *hnp_uri;
    int master;
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
    
    { NULL, NULL, NULL, '\0', "uri", "uri", 1,
      &my_globals.hnp_uri, OPAL_CMD_LINE_TYPE_STRING,
      "The uri of the ORCM master [for TCP multicast only]" },
    
    { NULL, NULL, NULL, 'm', "master", "master", 1,
      &my_globals.master, OPAL_CMD_LINE_TYPE_INT,
      "ID of ORCM master to be contacted" },
    
    /* End of list */
    { NULL, NULL, NULL, '\0', NULL, NULL, 0,
      NULL, OPAL_CMD_LINE_TYPE_NULL,
      NULL }
};

/*
 * Local variables & functions
 */
static opal_mutex_t lock;
static opal_condition_t cond;
static bool waiting=true;

static void pretty_print(opal_buffer_t *buf);

static void ps_recv(int status,
                    orte_process_name_t *sender,
                    orcm_pnp_tag_t tag,
                    opal_buffer_t *buf, void *cbdata);

/* update data function */
static void update_data(int fd, short flg, void *arg)
{
    opal_buffer_t buf;
    opal_event_t *tmp;
    struct timeval now;
    int32_t ret;
    time_t mytime;
    orcm_tool_cmd_t flag = OPENRCM_TOOL_PS_CMD;

    if (NULL != arg) {
        /* print a separator for the next output */
        fprintf(stderr, "\n=========================================================\n");
        time(&mytime);
        fprintf(stderr, "Time: %s\n", ctime(&mytime));
    }

    /* setup the buffer to send our cmd */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    
    if (ORTE_SUCCESS != (ret = opal_dss.pack(&buf, &flag, 1, OPENRCM_TOOL_CMD_T))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    if (ORCM_SUCCESS != (ret = orcm_pnp.output_buffer(ORCM_PNP_SYS_CHANNEL,
                                                      NULL, ORCM_PNP_TAG_TOOL,
                                                      &buf))) {
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
        opal_evtimer_add(tmp, &now);
    }
}

int main(int argc, char *argv[])
{
    int32_t ret;
    opal_cmd_line_t cmd_line;
    char *args = NULL;
    char *mstr;
    
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
    my_globals.hnp_uri = NULL;
    my_globals.master = -1;

    OBJ_CONSTRUCT(&lock, opal_mutex_t);
    OBJ_CONSTRUCT(&cond, opal_condition_t);
    
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
    
    /* need to specify master */
    if (my_globals.master < 0) {
        opal_output(0, "Must specify ORCM master");
        return ORTE_ERROR;
    }
    asprintf(&mstr, "OMPI_MCA_orte_ess_job_family=%d", my_globals.master);
    putenv(mstr);
    
    /* if we were given HNP contact info, parse it and
     * setup the process_info struct with that info
     */
    if (NULL != my_globals.hnp_uri) {
        if (0 == strncmp(my_globals.hnp_uri, "file", strlen("file")) ||
            0 == strncmp(my_globals.hnp_uri, "FILE", strlen("FILE"))) {
            char input[1024], *filename;
            FILE *fp;
            
            /* it is a file - get the filename */
            filename = strchr(my_globals.hnp_uri, ':');
            if (NULL == filename) {
                /* filename is not correctly formatted */
                orte_show_help("help-openrcm-runtime.txt", "hnp-filename-bad", true, "uri", my_globals.hnp_uri);
                goto cleanup;
            }
            ++filename; /* space past the : */
            
            if (0 >= strlen(filename)) {
                /* they forgot to give us the name! */
                orte_show_help("help-openrcm-runtime.txt", "hnp-filename-bad", true, "uri", my_globals.hnp_uri);
                goto cleanup;
            }
            
            /* open the file and extract the uri */
            fp = fopen(filename, "r");
            if (NULL == fp) { /* can't find or read file! */
                orte_show_help("help-openrcm-runtime.txt", "hnp-filename-access", true, filename);
                goto cleanup;
            }
            if (NULL == fgets(input, 1024, fp)) {
                /* something malformed about file */
                fclose(fp);
                orte_show_help("help-openrcm-runtime.txt", "hnp-file-bad", true, filename);
                goto cleanup;
            }
            fclose(fp);
            input[strlen(input)-1] = '\0';  /* remove newline */
            /* put into the process info struct */
            orte_process_info.my_hnp_uri = strdup(input);
        } else {
            /* should just be the uri itself */
            orte_process_info.my_hnp_uri = strdup(my_globals.hnp_uri);
        }
    }
    
    /***************************
     * We need all of OPAL and ORTE - this will
     * automatically connect us to the CM
     ***************************/
    if (ORCM_SUCCESS != (ret = orcm_init(OPENRCM_TOOL))) {
        goto cleanup;
    }

    /* register to receive responses */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_input_buffer("orcm", "0.1", "alpha",
                                                              ORCM_PNP_SYS_CHANNEL,
                                                              ORCM_PNP_TAG_TOOL,
                                                              ps_recv))) {
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
    
    opal_event_dispatch();
    
    /***************
     * Cleanup
     ***************/
 cleanup:
    /* cancel the recv */
    orcm_pnp.deregister_input("orcm", "0.1", "alpha",
                              ORCM_PNP_SYS_CHANNEL,
                              ORCM_PNP_TAG_TOOL);
    
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
                    opal_buffer_t *buf, void *cbdata)
{
    orcm_tool_cmd_t flag;
    char *app;
    int32_t n, j;
    orte_vpid_t vpid;
    char *node;
    int rc;

    /* unpack the cmd */
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &flag, &n, OPENRCM_TOOL_CMD_T))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    /* if this isn't a response to us, ignore it */
    if (OPENRCM_TOOL_PS_CMD != flag) {
        return;
    }
    
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

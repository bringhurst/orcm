/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

/**
 * @fie
 * Open Cluster Manager support tool
 *
 */

/* add the openrcm definitions */
#include "../runtime/runtime.h"

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

#include "orte/runtime/runtime.h"
#include "orte/util/show_help.h"
#include "orte/util/parse_options.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"
#include "orte/mca/rml/rml.h"
#include "orte/util/name_fns.h"
#include "orte/mca/odls/odls_types.h"
#include "orte/mca/rmcast/rmcast.h"

/*****************************************
 * Global Vars for Command line Arguments
 *****************************************/
static struct {
    bool help;
    bool monitor;
    int update_rate;
    char *scope;
    int scope_flag;
    char *hnp_uri;
} my_globals;

opal_cmd_line_init_t cmd_line_opts[] = {
    { NULL, NULL, NULL, 'h', NULL, "help", 0,
      &my_globals.help, OPAL_CMD_LINE_TYPE_BOOL,
      "Print help message" },

    { NULL, NULL, NULL, 'm', "monitor", "monitor", 0,
      &my_globals.monitor, OPAL_CMD_LINE_TYPE_BOOL,
      "Provide a regularly updated picture of the system (default: single snapshot)" },
    
    { NULL, NULL, NULL, '\0', "scope", "scope", 1,
      &my_globals.scope, OPAL_CMD_LINE_TYPE_STRING,
      "Scope of the data [CM | node | app] (default: CM)" },
    
    { NULL, NULL, NULL, '\0', "update-rate", "update-rate", 1,
      &my_globals.update_rate, OPAL_CMD_LINE_TYPE_INT,
      "Update rate in seconds (default: 5)" },
    
    { NULL, NULL, NULL, '\0', "uri", "uri", 1,
      &my_globals.hnp_uri, OPAL_CMD_LINE_TYPE_STRING,
      "The uri of the CM that you wish to query/monitor" },

    /* End of list */
    { NULL, NULL, NULL, '\0', NULL, NULL, 0,
      NULL, OPAL_CMD_LINE_TYPE_NULL,
      NULL }
};

/*
 * Local variables & functions
 */
static void abort_exit_callback(int fd, short flags, void *arg);
static struct opal_event term_handler;
static struct opal_event int_handler;
static opal_event_t *orted_exit_event;

static void pretty_print(opal_buffer_t *buf);

static void ps_recv(int status,
                    int channel, orte_rmcast_tag_t tag,
                    orte_process_name_t *sender,
                    orte_rmcast_seq_t seq_num,
                    opal_buffer_t *buf, void *cbdata);

/* update data function */
static void update_data(int fd, short flg, void *arg)
{
    opal_buffer_t buf;
    opal_event_t *tmp;
    struct timeval now;
    int32_t ret;
    orte_daemon_cmd_flag_t cmd;
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

    switch (my_globals.scope_flag) {
        case 1:
            /* point-to-point query of the CM */
            if (ORTE_SUCCESS != (ret = opal_dss.pack(&buf, &flag, 1, OPENRCM_TOOL_CMD_T))) {
                ORTE_ERROR_LOG(ret);
                goto cleanup;
            }
            
            if (0 > (ret = orte_rml.send_buffer(ORTE_PROC_MY_HNP, &buf, ORTE_RML_TAG_TOOL, 0))) {
                ORTE_ERROR_LOG(ret);
                goto cleanup;
            }
            OBJ_DESTRUCT(&buf);
            
            /* now wait for info */
            OBJ_CONSTRUCT(&buf, opal_buffer_t);
            if (0 > (ret = orte_rml.recv_buffer(ORTE_NAME_WILDCARD, &buf, ORTE_RML_TAG_TOOL, 0))) {
                ORTE_ERROR_LOG(ret);
                goto cleanup;
            }
            pretty_print(&buf);
            break;
        
        case 2:
            /* multicast query of the daemons, including the CM - we
             * don't need anything in the buffer as the cmd's arrival
             * is enough to trigger the response
             */
            if (ORTE_SUCCESS != (ret = orte_rmcast.send_buffer(ORTE_RMCAST_SYS_CHANNEL,
                                                               ORTE_RMCAST_TAG_PS,
                                                               &buf))) {
                ORTE_ERROR_LOG(ret);
            }
            break;
        
        case 3:
            /* multicast query for info from the apps themselves,
             * including who their pnp leader is
             */
            break;
        
        default:
            break;
    }
    
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
    my_globals.scope = NULL;
    my_globals.update_rate = 5;
    my_globals.hnp_uri = NULL;
    
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
    
    if (NULL == my_globals.scope || 0 == strcasecmp(my_globals.scope, "cm")) {
        my_globals.scope_flag = 1;
    } else if (0 == strcasecmp(my_globals.scope, "node")) {
        my_globals.scope_flag = 2;
    } else if (0 == strcasecmp(my_globals.scope, "app")) {
        my_globals.scope_flag = 3;
    } else {
        fprintf(stderr, "Unknown value %s for scope\n", my_globals.scope);
        args = opal_cmd_line_get_usage_msg(&cmd_line);
        orte_show_help("help-orcm-ps.txt", "usage", true, args);
        free(args);
        return ORTE_ERROR;
    }
    
    /* setup the exit triggers */
    OBJ_CONSTRUCT(&orte_exit, orte_trigger_event_t);
    OBJ_CONSTRUCT(&orteds_exit, orte_trigger_event_t);

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
    if (ORTE_SUCCESS != (ret = orcm_init(OPENRCM_TOOL))) {
        goto cleanup;
    }

    /* we know we need to print the data once */
    update_data(0, 0, NULL);
    
    if (my_globals.monitor) {
        if (1 < my_globals.scope_flag) {
            /* if we are using multicast, setup the recv */
            if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(ORTE_RMCAST_SYS_CHANNEL,
                                                                  ORTE_RMCAST_TAG_PS,
                                                                  ORTE_RMCAST_PERSISTENT,
                                                                  ps_recv, NULL))) {
                ORTE_ERROR_LOG(ret);
                goto cleanup;
            }
        }
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
    /* Remove the TERM and INT signal handlers */
    opal_signal_del(&term_handler);
    opal_signal_del(&int_handler);
    OBJ_DESTRUCT(&orte_exit);

    /* cleanup orte */
    
    orcm_finalize();

    return ret;
}

static void abort_exit_callback(int fd, short ign, void *arg)
{
    opal_list_item_t *item;
    int ret;
    
    /* Remove the TERM and INT signal handlers */
    opal_signal_del(&term_handler);
    opal_signal_del(&int_handler);
    OBJ_DESTRUCT(&orte_exit);

    orcm_finalize();
    exit(1);
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
                    int channel, orte_rmcast_tag_t tag,
                    orte_process_name_t *sender,
                    orte_rmcast_seq_t seq_num,
                    opal_buffer_t *buf, void *cbdata)
{
    char *app;
    int32_t n, j;
    orte_vpid_t vpid;
    char *node;
    int rc;

    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &node, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
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
            return;
        }
        fprintf(stderr, "      Instances: ");
        while (ORTE_VPID_INVALID != vpid) {
            fprintf(stderr, "%s ", ORTE_VPID_PRINT(vpid));
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &vpid, &n, ORTE_VPID))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");
}

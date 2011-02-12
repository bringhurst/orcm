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

#include "opal/event/event.h"
#include "opal/util/cmd_line.h"
#include "opal/util/argv.h"
#include "opal/util/opal_environ.h"
#include "opal/util/path.h"
#include "opal/mca/base/base.h"
#include "opal/mca/base/mca_base_param.h"
#include "opal/runtime/opal.h"

#include "orte/util/show_help.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"
#include "orte/util/name_fns.h"
#include "orte/util/proc_info.h"

#include "mca/pnp/pnp.h"

/*****************************************
 * Global Vars for Command line Arguments
 *****************************************/
static struct {
    bool help;
    char *replicas;
    char *sched;
    char *hnp_uri;
} my_globals;

opal_cmd_line_init_t cmd_line_opts[] = {
    { NULL, NULL, NULL, 'h', NULL, "help", 0,
        &my_globals.help, OPAL_CMD_LINE_TYPE_BOOL,
        "Print help message" },
    
    { NULL, NULL, NULL, 'r', "replica", "replica", 1,
      &my_globals.replicas, OPAL_CMD_LINE_TYPE_STRING,
      "Comma-separated range(s) of replicas to be stopped" },
    
    { NULL, NULL, NULL, 'd', "dvm", "dvm", 1,
      &my_globals.sched, OPAL_CMD_LINE_TYPE_STRING,
      "ORCM DVM to be contacted" },
    
    { NULL, NULL, NULL, '\0', "uri", "uri", 1,
      &my_globals.hnp_uri, OPAL_CMD_LINE_TYPE_STRING,
      "The uri of the ORCM DVM [uri or file:name of file containing it" },
    
    /* End of list */
    { NULL, NULL, NULL, '\0', NULL, NULL, 
      0, NULL, OPAL_CMD_LINE_TYPE_NULL,
      NULL }
};

/*
 * Local variables & functions
 */
static void ack_recv(int status,
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

int main(int argc, char *argv[])
{
    int32_t ret, i;
    opal_cmd_line_t cmd_line;
    char **inpt;
    opal_buffer_t *buf;
    int count;
    char cwd[OPAL_PATH_MAX];
    orcm_tool_cmd_t flag = ORCM_TOOL_STOP_CMD;
    int32_t master=0;
    uint16_t jfam=0;

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
    my_globals.replicas = NULL;
    my_globals.sched = NULL;
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
        char *args = NULL;
        args = opal_cmd_line_get_usage_msg(&cmd_line);
        orte_show_help("help-orcm-stop.txt", "usage", true, args);
        free(args);
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
            /* convert the pid */
            master = strtoul(input, NULL, 10);
        } else {
            /* should just be the master itself */
            master = strtoul(my_globals.sched, NULL, 10);
        }
    }

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
    
    if (OPAL_SUCCESS != opal_getcwd(cwd, sizeof(cwd))) {
        opal_output(orte_clean_output, "failed to get cwd\n");
        return ORTE_ERR_NOT_FOUND;
    }
    
    /***************************
     * We need all of OPAL and ORTE - this will
     * automatically connect us to the CM
     ***************************/
    if (ORTE_SUCCESS != orcm_init(ORCM_TOOL)) {
        orcm_finalize();
        return 1;
    }
    
    /* if we were given the hnp uri, extract the job family for the
     * master id
     */
    if (NULL != my_globals.hnp_uri) {
        master = ORTE_JOB_FAMILY(ORTE_PROC_MY_HNP->jobid);
    }
    
    /* register to receive responses */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcm-stop", "0.1", "alpha",
                                                         ORCM_PNP_GROUP_INPUT_CHANNEL,
                                                         ORCM_PNP_TAG_TOOL,
                                                         ack_recv, NULL))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    /* announce my existence */
    if (ORCM_SUCCESS != (ret = orcm_pnp.announce("orcm-stop", "0.1", "alpha", NULL))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    /* setup the buffer to send our cmd */
    buf = OBJ_NEW(opal_buffer_t);
    
    /* indicate the scheduler to be used */
    jfam = master & 0x0000ffff;
    opal_dss.pack(buf, &jfam, 1, OPAL_UINT16);
    
    /* get the apps to stop */
    inpt = NULL;
    opal_cmd_line_get_tail(&cmd_line, &count, &inpt);
    
    if (0 == count) {
        /* if no apps were given, then we stop the entire
         * DVM itself by telling the daemon's to terminate
         */
        if (ORCM_SUCCESS != (ret = orcm_pnp.output_nb(ORCM_PNP_SYS_CHANNEL,
                                                      NULL, ORCM_PNP_TAG_TERMINATE,
                                                      NULL, 0, buf, cbfunc, NULL))) {
            ORTE_ERROR_LOG(ret);
        }
        goto cleanup;
    } else {
        /* load the stop cmd */
        opal_dss.pack(buf, &flag, 1, ORCM_TOOL_CMD_T);
    
        /* for each app */
        for (i=0; NULL != inpt[i]; i++) {
            opal_dss.pack(buf, &inpt[i], 1, OPAL_STRING);
            /* pack the replicas to be stopped */
            opal_dss.pack(buf, &my_globals.replicas, 1, OPAL_STRING);
        }
        opal_argv_free(inpt);
    
        if (ORCM_SUCCESS != (ret = orcm_pnp.output_nb(ORCM_PNP_SYS_CHANNEL,
                                                      NULL, ORCM_PNP_TAG_TOOL,
                                                      NULL, 0, buf, cbfunc, NULL))) {
            ORTE_ERROR_LOG(ret);
        }
    }

    /* now wait for ack */
    opal_event_dispatch();
    
    /***************
     * Cleanup
     ***************/
 cleanup:
    orcm_finalize();
    
    return ret;
}

static void ack_recv(int status,
                     orte_process_name_t *sender,
                     orcm_pnp_tag_t tag,
                     struct iovec *msg, int count,
                     opal_buffer_t *buf, void *cbdata)
{
    int rc, n;
    orcm_tool_cmd_t flag;

    /* unpack the cmd and verify it is us */
    n=1;
    opal_dss.unpack(buf, &flag, &n, ORCM_TOOL_CMD_T);
    if (ORCM_TOOL_STOP_CMD != flag) {
        /* wrong cmd */
        opal_output(0, "GOT WRONG CMD");
        return;
    }

    /* unpack the result of the stop/term command */
    n=1;
    opal_dss.unpack(buf, &rc, &n, OPAL_INT);
    ORTE_UPDATE_EXIT_STATUS(rc);

    if (0 == rc) {
        opal_output(orte_clean_output, "Job stopped");
    } else {
        opal_output(orte_clean_output, "Job failed to stop with error %s", ORTE_ERROR_NAME(rc));
    }

    /* the fact we recvd this is enough */
    orcm_finalize();
    exit(0);
}

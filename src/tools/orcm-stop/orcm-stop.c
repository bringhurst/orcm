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
#include "opal/threads/threads.h"

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
} my_globals;

opal_cmd_line_init_t cmd_line_opts[] = {
    { NULL, NULL, NULL, 'h', NULL, "help", 0,
        &my_globals.help, OPAL_CMD_LINE_TYPE_BOOL,
        "Print help message" },
    
    { NULL, NULL, NULL, 'r', "replica", "replica", 1,
      &my_globals.replicas, OPAL_CMD_LINE_TYPE_STRING,
      "Comma-separated range(s) of replicas to be stopped" },
    
    { NULL, NULL, NULL, 's', "sched", "sched", 1,
      &my_globals.sched, OPAL_CMD_LINE_TYPE_STRING,
      "ORCM scheduler to be contacted" },
    
    /* End of list */
    { NULL, NULL, NULL, '\0', NULL, NULL, 
      0, NULL, OPAL_CMD_LINE_TYPE_NULL,
      NULL }
};

/*
 * Local variables & functions
 */
static opal_mutex_t lock;
static opal_condition_t cond;
static bool waiting=true;

static void ack_recv(int status,
                     orte_process_name_t *sender,
                     orcm_pnp_tag_t tag,
                     opal_buffer_t *buf, void *cbdata);


int main(int argc, char *argv[])
{
    int32_t ret, i;
    opal_cmd_line_t cmd_line;
    char *cmd;
    char **inpt;
    opal_buffer_t buf;
    int count;
    char cwd[OPAL_PATH_MAX];
    orcm_tool_cmd_t flag = ORCM_TOOL_STOP_CMD;
    int32_t master;
    uint16_t jfam;

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
        char *args = NULL;
        args = opal_cmd_line_get_usage_msg(&cmd_line);
        orte_show_help("help-orcm-stop.txt", "usage", true, args);
        free(args);
        return ORTE_ERROR;
    }
    
    /* need to specify master */
    if (NULL == my_globals.sched) {
        opal_output(0, "Must specify ORCM scheduler to be contacted");
        return ORTE_ERROR;
    }
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
    
    /* register to receive responses */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_input_buffer("orcm", "0.1", "alpha",
                                                              ORCM_PNP_SYS_CHANNEL,
                                                              ORCM_PNP_TAG_TOOL,
                                                              ack_recv))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    /* setup the buffer to send our cmd */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    
    /* indicate the scheduler to be used */
    jfam = master & 0x0000ffff;
    opal_dss.pack(&buf, &jfam, 1, OPAL_UINT16);
    
    /* load the stop cmd */
    opal_dss.pack(&buf, &flag, 1, ORCM_TOOL_CMD_T);
    
    /* get the apps to stop */
    inpt = NULL;
    opal_cmd_line_get_tail(&cmd_line, &count, &inpt);
    
    /* for each app */
    for (i=0; NULL != inpt[i]; i++) {
        /* get the absolute path */
        if (NULL == (cmd = opal_find_absolute_path(inpt[i]))) {
            opal_output(orte_clean_output, "App %s could not be found - try changing path\n", inpt[i]);
            opal_argv_free(inpt);
            OBJ_DESTRUCT(&buf);
            goto cleanup;
        }
        opal_dss.pack(&buf, &cmd, 1, OPAL_STRING);
        free(cmd);
        /* pack the replicas to be stopped */
        opal_dss.pack(&buf, &my_globals.replicas, 1, OPAL_STRING);
    }
    opal_argv_free(inpt);
    
    if (ORCM_SUCCESS != (ret = orcm_pnp.output_buffer(ORCM_PNP_SYS_CHANNEL,
                                                      NULL, ORCM_PNP_TAG_TOOL,
                                                      &buf))) {
        ORTE_ERROR_LOG(ret);
    }
    OBJ_DESTRUCT(&buf);
    
    /* now wait for ack */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &waiting);
    
    /***************
     * Cleanup
     ***************/
cleanup:
    OBJ_DESTRUCT(&lock);
    OBJ_DESTRUCT(&cond);
    orcm_finalize();
    
    return ret;
}

static void ack_recv(int status,
                     orte_process_name_t *sender,
                     orcm_pnp_tag_t tag,
                     opal_buffer_t *buf, void *cbdata)
{
    orcm_tool_cmd_t flag;
    int32_t n;
    int rc;
    opal_buffer_t *ans;
    uint16_t jfam;
    
    /* if it isn't for me, ignore it */
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &jfam, &n, OPAL_UINT16))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    if (jfam != ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid)) {
        opal_output(0, "%s NOT FOR ME!", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        return;
    }

    /* unpack the cmd */
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &flag, &n, ORCM_TOOL_CMD_T))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    /* if this isn't a response to us, ignore it */
    if (ORCM_TOOL_STOP_CMD != flag) {
        return;
    }
    
    /* release the wait */
    OPAL_RELEASE_THREAD(&lock, &cond, &waiting);
}

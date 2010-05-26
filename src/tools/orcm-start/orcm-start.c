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

#include "orte/runtime/runtime.h"
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
    int num_procs;
    int add_procs;
    char *hosts;
    bool constrained;
    bool gdb;
    int max_restarts;
    char *sched;
    char *hnp_uri;
    bool continuous;
} my_globals;

opal_cmd_line_init_t cmd_line_opts[] = {
    { NULL, NULL, NULL, 'h', NULL, "help", 0,
      &my_globals.help, OPAL_CMD_LINE_TYPE_BOOL,
      "Print help message" },

    { NULL, NULL, NULL, 'n', "np", "np", 1,
      &my_globals.num_procs, OPAL_CMD_LINE_TYPE_INT,
      "Number of instances to start" },

    { NULL, NULL, NULL, 'a', "add", "add", 1,
      &my_globals.add_procs, OPAL_CMD_LINE_TYPE_BOOL,
      "Number of instances to be added to an existing job" },
    
    { NULL, NULL, NULL, '\0', "gdb", "gdb", 0,
      &my_globals.gdb, OPAL_CMD_LINE_TYPE_BOOL,
      "Have the application spin in init until a debugger attaches" },

    { NULL, NULL, NULL, 'H', "host", "host", 1,
      &my_globals.hosts, OPAL_CMD_LINE_TYPE_STRING,
      "Comma-delimited list of hosts upon which procs are to be initially started" },

    { NULL, NULL, NULL, '\0', "constrain", "constrain", 0,
      &my_globals.constrained, OPAL_CMD_LINE_TYPE_BOOL,
      "Constrain processes to run solely on the specified hosts, even upon restart from failure" },

    { NULL, NULL, NULL, 'r', "max-restarts", "max-restarts", 1,
      &my_globals.max_restarts, OPAL_CMD_LINE_TYPE_INT,
      "Maximum number of times a process in this job can be restarted (default: unbounded)" },

    { NULL, NULL, NULL, 'c', "continuous", "continuous", 0,
      &my_globals.continuous, OPAL_CMD_LINE_TYPE_BOOL,
      "Restart processes even if they terminate normally (i.e., return zero status)" },
    
    { NULL, NULL, NULL, 'd', "dvm", "dvm", 1,
      &my_globals.sched, OPAL_CMD_LINE_TYPE_STRING,
      "ORCM DVM to be contacted [number or file:name of file containing it" },
    
    { NULL, NULL, NULL, '\0', "uri", "uri", 1,
      &my_globals.hnp_uri, OPAL_CMD_LINE_TYPE_STRING,
      "The uri of the ORCM DVM [uri or file:name of file containing it" },
    
    /* End of list */
    { NULL, NULL, NULL, 
      '\0', NULL, NULL, 
      0,
      NULL, OPAL_CMD_LINE_TYPE_NULL,
      NULL }
};

static opal_mutex_t lock;
static opal_condition_t cond;
static bool waiting=true;

static void ack_recv(int status,
                     orte_process_name_t *sender,
                     orcm_pnp_tag_t tag,
                     opal_buffer_t *buf, void *cbdata);


int main(int argc, char *argv[])
{
    int32_t ret, i, num_apps, restarts;
    opal_cmd_line_t cmd_line;
    FILE *fp;
    char *cmd, *mstr;
    char **inpt, **xfer;
    opal_buffer_t buf;
    int count;
    char cwd[OPAL_PATH_MAX];
    char *app;
    orcm_tool_cmd_t flag = ORCM_TOOL_START_CMD;
    int8_t constrain;
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
        fprintf(stderr, "Failed to init orcm util\n");
        return ret;
    }
    
    /* initialize the globals */
    my_globals.help = false;
    my_globals.num_procs = 0;
    my_globals.hosts = NULL;
    my_globals.constrained = false;
    my_globals.add_procs = 0;
    my_globals.gdb = false;
    my_globals.max_restarts = -1;
    my_globals.sched = NULL;
    my_globals.hnp_uri = NULL;
    my_globals.continuous = false;
    
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
        orte_show_help("help-orcm-start.txt", "usage", true, args);
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
            
            /* open the file and extract the job family */
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
            /* convert the job family */
            master = strtoul(input, NULL, 10);
        } else {
            /* should just be the scheduler itself */
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
            input[strlen(input)-1] = '\0';  /* remove newline */
            /* put into the process info struct */
            orte_process_info.my_hnp_uri = strdup(input);
        } else {
            /* should just be the uri itself */
            orte_process_info.my_hnp_uri = strdup(my_globals.hnp_uri);
        }
    }

    /* bozo check - cannot specify both add and num procs */
    if (0 < my_globals.add_procs && 0 < my_globals.num_procs) {
        opal_output(0, "Cannot specify both -a and -n options together");
        return ORTE_ERROR;
    }
    if (my_globals.add_procs < 0 || my_globals.num_procs < 0) {
        opal_output(0, "Error - negative number of procs given");
        return ORTE_ERROR;
    }
    
    /* get the current working directory */
    if (OPAL_SUCCESS != opal_getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "failed to get cwd\n");
        return ORTE_ERR_NOT_FOUND;
    }
    
    /* setup the max number of restarts */
    if (-1 == my_globals.max_restarts) {
        restarts = INT32_MAX;
    } else {
        restarts = my_globals.max_restarts;
    }
    
    /***************************
     * We need all of OPAL and ORTE - this will
     * automatically connect us to the CM
     ***************************/
    if (ORTE_SUCCESS != orcm_init(ORCM_TOOL)) {
        fprintf(stderr, "Failed orcm_init\n");
        orte_finalize();
        return 1;
    }

    /* if we were given the hnp uri, extract the job family for the
     * master id
     */
    if (NULL != my_globals.hnp_uri) {
        master = ORTE_JOB_FAMILY(ORTE_PROC_MY_HNP->jobid);
    }
    
    /* register to receive responses */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_input_buffer("orcm-start", "0.1", "alpha",
                                                              ORCM_PNP_TAG_TOOL,
                                                              ack_recv))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    /* announce my existence */
    if (ORCM_SUCCESS != (ret = orcm_pnp.announce("orcm-start", "0.1", "alpha", NULL))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    
    /* setup the buffer to send our cmd */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);

    /* indicate the scheduler to be used */
    jfam = master & 0x0000ffff;
    opal_output(0, "JOB FAMILY %04x", jfam);
    
    opal_dss.pack(&buf, &jfam, 1, OPAL_UINT16);
    
    /* load the start cmd */
    opal_dss.pack(&buf, &flag, 1, ORCM_TOOL_CMD_T);
    
    /* load the add procs flag */
    if (0 < my_globals.add_procs) {
        constrain = 1;
        my_globals.num_procs = my_globals.add_procs;
    } else {
        constrain = 0;
    }
    opal_dss.pack(&buf, &constrain, 1, OPAL_INT8);
    
    /* load the gdb flag */
    if (my_globals.gdb) {
        constrain = 1;
    } else {
        constrain = 0;
    }
    opal_dss.pack(&buf, &constrain, 1, OPAL_INT8);

    /* load the continuous flag */
    if (my_globals.continuous) {
        constrain = 1;
    } else {
        constrain = 0;
    }
    opal_dss.pack(&buf, &constrain, 1, OPAL_INT8);
    
    /* load the max restarts value */
    opal_dss.pack(&buf, &restarts, 1, OPAL_INT32);
    
    /* pack the number of instances to start */
    num_apps = my_globals.num_procs;
    opal_dss.pack(&buf, &num_apps, 1, OPAL_INT32);
    
    /* pack the starting hosts - okay to pack a NULL string */
    opal_dss.pack(&buf, &my_globals.hosts, 1, OPAL_STRING);
    
    /* pack the constraining flag */
    if (my_globals.constrained) {
        constrain = 1;
    } else {
        constrain = 0;
    }
    opal_dss.pack(&buf, &constrain, 1, OPAL_INT8);
    
    /* get the things to start */
    inpt = NULL;
    opal_cmd_line_get_tail(&cmd_line, &count, &inpt);
    
    /* get the absolute path */
    if (NULL == (app = opal_find_absolute_path(inpt[0]))) {
        fprintf(stderr, "App %s could not be found - try changing path\n", inpt[0]);
        goto cleanup;
    }
    xfer = NULL;
    opal_argv_append_nosize(&xfer, app);
    for (i=1; NULL != inpt[i]; i++) {
        opal_argv_append_nosize(&xfer, inpt[i]);
    }
    opal_argv_free(inpt);
    cmd = opal_argv_join(xfer, ' ');
    opal_argv_free(xfer);
    opal_dss.pack(&buf, &cmd, 1, OPAL_STRING);
    free(cmd);
    
    opal_output(0, "sending command");
    if (ORCM_SUCCESS != (ret = orcm_pnp.output_buffer(ORCM_PNP_SYS_CHANNEL,
                                                      NULL, ORCM_PNP_TAG_TOOL,
                                                      &buf))) {
        ORTE_ERROR_LOG(ret);
    }
    
    OBJ_DESTRUCT(&buf);
    opal_output(0, "waiting for ack");
    
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
    /* the fact we recvd this is enough - release the wait */
    OPAL_RELEASE_THREAD(&lock, &cond, &waiting);
}

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

#include "opal/mca/event/event.h"
#include "opal/util/cmd_line.h"
#include "opal/util/argv.h"
#include "opal/util/opal_environ.h"
#include "opal/util/os_path.h"
#include "opal/util/path.h"
#include "opal/mca/base/base.h"
#include "opal/mca/base/mca_base_param.h"
#include "opal/runtime/opal.h"

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
    char *appfile;
    char *name;
    char *instance;
    int num_procs;
    char *wdir;
    char *path;
    char *hosts;
    bool constrained;
    bool gdb;
    int local_restarts;
    int global_restarts;
    char *sched;
    char *hnp_uri;
    bool continuous;
} my_globals;

opal_cmd_line_init_t cmd_line_opts[] = {
    { NULL, NULL, NULL, 'h', NULL, "help", 0,
      &my_globals.help, OPAL_CMD_LINE_TYPE_BOOL,
      "Print help message" },

    /* Use an appfile */
    { NULL, NULL, NULL, '\0', NULL, "app", 1,
      &my_globals.appfile, OPAL_CMD_LINE_TYPE_STRING,
      "Provide an appfile; ignore all other command line options" },

    /* Name of the job */
    { NULL, NULL, NULL, '\0', NULL, "app-name", 1,
      &my_globals.name, OPAL_CMD_LINE_TYPE_STRING,
      "Name of the application" },

    /* Name of the instance */
    { NULL, NULL, NULL, '\0', NULL, "instance", 1,
      &my_globals.instance, OPAL_CMD_LINE_TYPE_STRING,
      "Name of the instance of this application" },

    { NULL, NULL, NULL, 'n', "np", "np", 1,
      &my_globals.num_procs, OPAL_CMD_LINE_TYPE_INT,
      "Number of instances to start" },

    { NULL, NULL, NULL, '\0', "wdir", "wdir", 1,
      &my_globals.wdir, OPAL_CMD_LINE_TYPE_STRING,
      "Set the working directory of the started processes" },
    { NULL, NULL, NULL, '\0', "wd", "wd", 1,
      &my_globals.wdir, OPAL_CMD_LINE_TYPE_STRING,
      "Synonym for --wdir" },
    { NULL, NULL, NULL, '\0', "path", "path", 1,
      &my_globals.path, OPAL_CMD_LINE_TYPE_STRING,
      "PATH to be used to look for executables to start processes" },

    { NULL, NULL, NULL, '\0', "gdb", "gdb", 0,
      &my_globals.gdb, OPAL_CMD_LINE_TYPE_BOOL,
      "Have the application spin in init until a debugger attaches" },

    { NULL, NULL, NULL, 'H', "host", "host", 1,
      &my_globals.hosts, OPAL_CMD_LINE_TYPE_STRING,
      "Comma-delimited list of hosts upon which procs are to be initially started" },

    { NULL, NULL, NULL, '\0', "constrain", "constrain", 0,
      &my_globals.constrained, OPAL_CMD_LINE_TYPE_BOOL,
      "Constrain processes to run solely on the specified hosts, even upon restart from failure" },

    { NULL, NULL, NULL, '\0', "max-local-restarts", "max-local-restarts", 1,
      &my_globals.local_restarts, OPAL_CMD_LINE_TYPE_INT,
      "Maximum number of times a process in this job can be restarted on the same node (default: unbounded)" },

    { NULL, NULL, NULL, '\0', "max-global-restarts", "max-global-restarts", 1,
      &my_globals.global_restarts, OPAL_CMD_LINE_TYPE_INT,
      "Maximum number of times a process in this job can be relocated to another node (default: unbounded)" },

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

static char **global_mca_env = NULL;
static orte_std_cntr_t total_num_apps = 0;
static char *app_launched=NULL;
static bool have_zero_np = false;

static void ack_recv(int status,
                     orte_process_name_t *sender,
                     orcm_pnp_tag_t tag,
                     struct iovec *msg, int count,
                     opal_buffer_t *buf, void *cbdata);
static void parse_apps(orte_job_t *jdata, int argc, char *argv[]);
static int create_app(int argc, char* argv[], orte_app_context_t **app,
                      bool *made_app, char ***app_env, orte_job_t *jdata);
static int parse_appfile(opal_buffer_t *buf);
static int capture_cmd_line_params(int argc, int start, char **argv, char***app_env);


int main(int argc, char *argv[])
{
    int32_t ret, i;
    opal_cmd_line_t cmd_line;
    FILE *fp;
    char *cmd, *mstr;
    char **inpt;
    opal_buffer_t buf;
    int count;
    char cwd[OPAL_PATH_MAX];
    orcm_tool_cmd_t flag = ORCM_TOOL_START_CMD;
    int32_t master;
    uint16_t jfam;
    orte_job_t *jdata;
    orte_app_context_t *app;
    
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
    my_globals.appfile = NULL;
    my_globals.name = NULL;
    my_globals.instance = NULL;
    my_globals.num_procs = 0;
    my_globals.wdir = NULL;
    my_globals.path = NULL;
    my_globals.hosts = NULL;
    my_globals.constrained = false;
    my_globals.gdb = false;
    my_globals.local_restarts = -1;
    my_globals.global_restarts = -1;
    my_globals.sched = NULL;
    my_globals.hnp_uri = NULL;
    my_globals.continuous = false;
    
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

    /* get the current working directory */
    if (OPAL_SUCCESS != opal_getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "failed to get cwd\n");
        return ORTE_ERR_NOT_FOUND;
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
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcm-start", "0.1", "alpha",
                                                         ORCM_PNP_GROUP_INPUT_CHANNEL,
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
    
    opal_dss.pack(&buf, &jfam, 1, OPAL_UINT16);
    
    /* load the start cmd */
    opal_dss.pack(&buf, &flag, 1, ORCM_TOOL_CMD_T);
    
    if (NULL != my_globals.appfile) {
        if (ORCM_SUCCESS != parse_appfile(&buf)) {
            goto cleanup;
        }
    } else {
        /* setup the job object to be launched */
        jdata = OBJ_NEW(orte_job_t);
        if (NULL != my_globals.name) {
            jdata->name = strdup(my_globals.name);
        }
        if (NULL != my_globals.instance) {
            jdata->instance = strdup(my_globals.instance);
        }

        /* set the spin flag */
        if (my_globals.gdb) {
            jdata->controls |= ORTE_JOB_CONTROL_SPIN_FOR_DEBUG;
        }

        /* set the continuous flag */
        if (my_globals.continuous) {
            jdata->controls |= ORTE_JOB_CONTROL_CONTINUOUS_OP;
        }

        /* setup the application(s) */
        parse_apps(jdata, argc, argv);

        /* if no apps were given, then abort */
        if (0 == jdata->num_apps) {
            opal_output(0, "NO APPS GIVEN");
            goto cleanup;
        }

        /* if it wasn't given, set the default app name */
        if (NULL == jdata->name) {
            app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, 0);
            jdata->name = strdup(app->app);
        }
        /* save the job's name */
        app_launched = strdup(jdata->name);

        /* pack the job object */
        if (ORTE_SUCCESS != (ret = opal_dss.pack(&buf, &jdata, 1, ORTE_JOB))) {
            ORTE_ERROR_LOG(ret);
            OBJ_RELEASE(jdata);
            OBJ_DESTRUCT(&buf);
            goto cleanup;
        }
        /* done with this */
        OBJ_RELEASE(jdata);
    }

    if (ORCM_SUCCESS != (ret = orcm_pnp.output(ORCM_PNP_GROUP_OUTPUT_CHANNEL,
                                               NULL, ORCM_PNP_TAG_TOOL,
                                               NULL, 0, &buf))) {
        ORTE_ERROR_LOG(ret);
    }
    
    OBJ_DESTRUCT(&buf);
    
    /* now wait for ack */
    opal_event_dispatch(opal_event_base);
    
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
    if (ORCM_TOOL_START_CMD != flag && ORCM_TOOL_ILLEGAL_CMD != flag) {
        /* wrong cmd */
        opal_output(0, "GOT WRONG CMD");
        return;
    }

    /* unpack the result of the start command */
    n=1;
    opal_dss.unpack(buf, &rc, &n, OPAL_INT);
    ORTE_UPDATE_EXIT_STATUS(rc);

    if (0 == rc) {
        opal_output(orte_clean_output, "Job %s started", app_launched);
    } else {
        opal_output(orte_clean_output, "Job %s failed to start with error %s", app_launched, ORTE_ERROR_NAME(rc));
    }
    free(app_launched);

    /* the fact we recvd this is enough */
    exit(0);
}

static void parse_apps(orte_job_t *jdata, int argc, char *argv[])
{
    int i, rc, app_num;
    int temp_argc;
    char **temp_argv, **env;
    orte_app_context_t *app;
    bool made_app;
    orte_std_cntr_t j, size1;

    temp_argc = 0;
    temp_argv = NULL;
    opal_argv_append(&temp_argc, &temp_argv, argv[0]);

    /* NOTE: This bogus env variable is necessary in the calls to
       create_app(), below.  See comment immediately before the
       create_app() function for an explanation. */

    env = NULL;
    for (app_num = 0, i = 1; i < argc; ++i) {
        if (0 == strcmp(argv[i], ":")) {
            /* Make an app with this argv */
            if (opal_argv_count(temp_argv) > 1) {
                if (NULL != env) {
                    opal_argv_free(env);
                    env = NULL;
                }
                app = NULL;
                rc = create_app(temp_argc, temp_argv, &app, &made_app, &env, jdata);
                /** keep track of the number of apps - point this app_context to that index */
                if (ORTE_SUCCESS != rc) {
                    /* Assume that the error message has already been
                       printed; no need to cleanup -- we can just
                       exit */
                    exit(1);
                }
                if (made_app) {
                    app->idx = app_num;
                    ++app_num;
                    opal_pointer_array_add(jdata->apps, app);
                    ++jdata->num_apps;
                }

                /* Reset the temps */
                temp_argc = 0;
                temp_argv = NULL;
                opal_argv_append(&temp_argc, &temp_argv, argv[0]);
            }
        } else {
            opal_argv_append(&temp_argc, &temp_argv, argv[i]);
        }
    }

    if (opal_argv_count(temp_argv) > 1) {
        app = NULL;
        rc = create_app(temp_argc, temp_argv, &app, &made_app, &env, jdata);
        if (ORTE_SUCCESS != rc) {
            /* Assume that the error message has already been printed;
               no need to cleanup -- we can just exit */
            exit(1);
        }
        if (made_app) {
            app->idx = app_num;
            ++app_num;
            opal_pointer_array_add(jdata->apps, app);
            ++jdata->num_apps;
        }
    }
    if (NULL != env) {
        opal_argv_free(env);
    }
    opal_argv_free(temp_argv);

    /* Once we've created all the apps, add the global MCA params to
       each app's environment (checking for duplicates, of
       course -- yay opal_environ_merge()).  */

    if (NULL != global_mca_env) {
        size1 = (size_t)opal_pointer_array_get_size(jdata->apps);
        /* Iterate through all the apps */
        for (j = 0; j < size1; ++j) {
            app = (orte_app_context_t *)
                opal_pointer_array_get_item(jdata->apps, j);
            if (NULL != app) {
                /* Use handy utility function */
                env = opal_environ_merge(global_mca_env, app->env);
                opal_argv_free(app->env);
                app->env = env;
            }
        }
    }
}

/*
 * This function takes a "char ***app_env" parameter to handle the
 * specific case:
 *
 *   orterun --mca foo bar -app appfile
 *
 * That is, we'll need to keep foo=bar, but the presence of the app
 * file will cause an invocation of parse_appfile(), which will cause
 * one or more recursive calls back to create_app().  Since the
 * foo=bar value applies globally to all apps in the appfile, we need
 * to pass in the "base" environment (that contains the foo=bar value)
 * when we parse each line in the appfile.
 *
 * This is really just a special case -- when we have a simple case like:
 *
 *   orterun --mca foo bar -np 4 hostname
 *
 * Then the upper-level function (parse_locals()) calls create_app()
 * with a NULL value for app_env, meaning that there is no "base"
 * environment that the app needs to be created from.
 */
static int create_app(int argc, char* argv[], orte_app_context_t **app_ptr,
                      bool *made_app, char ***app_env, orte_job_t *jdata)
{
    opal_cmd_line_t cmd_line;
    char cwd[OPAL_PATH_MAX];
    int i, j, count, rc;
    char *param, *value, *value2;
    orte_app_context_t *app = NULL;
    bool cmd_line_made = false;

    *made_app = false;

    /* Parse application command line options. */

    opal_cmd_line_create(&cmd_line, cmd_line_opts);
    mca_base_cmd_line_setup(&cmd_line);
    cmd_line_made = true;
    rc = opal_cmd_line_parse(&cmd_line, true, argc, argv);
    if (ORTE_SUCCESS != rc) {
        opal_output(0, "FAILED TO PARSE CMD LINE");
        goto cleanup;
    }
    mca_base_cmd_line_process_args(&cmd_line, app_env, NULL);

    /* Setup application context */

    app = OBJ_NEW(orte_app_context_t);
    opal_cmd_line_get_tail(&cmd_line, &count, &app->argv);

    /* See if we have anything left */

    if (0 == count) {
        opal_output(0, "Must provide at least one application");
        goto cleanup;
    }

    /* set the max local restarts value */
    app->max_local_restarts = my_globals.local_restarts;
    
    /* set the max global restarts value */
    app->max_global_restarts = my_globals.global_restarts;

    /* set the starting hosts */
    if (NULL != my_globals.hosts) {
        app->dash_host = opal_argv_split(my_globals.hosts, ',');
    }
    
    /* set the constraining flag */
    if (my_globals.constrained) {
        app->constrain = true;
    }    

    /* Grab all OMPI_* environment variables */

    app->env = opal_argv_copy(*app_env);
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
    
#if 0
    /*
     * Get mca parameters so we can pass them to the app.
     * Use the count determined above to make sure we do not go past
     * the executable name. Example:
     *   mpirun -np 2 -mca foo bar ./my-app -mca bip bop
     * We want to pick up '-mca foo bar' but not '-mca bip bop'
     */
    if (ORTE_SUCCESS != (rc = capture_cmd_line_params(argc, count, argv, &app->env))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
#endif

    /* tell the app to use the right ess module */
    opal_setenv("OMPI_MCA_ess", "orcmapp", true, &app->env);

    /* Did the user request to export any environment variables? */

    if (opal_cmd_line_is_taken(&cmd_line, "x")) {
        j = opal_cmd_line_get_ninsts(&cmd_line, "x");
        for (i = 0; i < j; ++i) {
            param = opal_cmd_line_get_param(&cmd_line, "x", i, 0);

            if (NULL != strchr(param, '=')) {
                opal_argv_append_nosize(&app->env, param);
            } else {
                value = getenv(param);
                if (NULL != value) {
                    if (NULL != strchr(value, '=')) {
                        opal_argv_append_nosize(&app->env, value);
                    } else {
                        asprintf(&value2, "%s=%s", param, value);
                        opal_argv_append_nosize(&app->env, value2);
                        free(value2);
                    }
                } else {
                    opal_output(0, "Warning: could not find environment variable \"%s\"\n", param);
                }
            }
        }
    }

    /* If the user specified --path, store it in the user's app
       environment via the OMPI_exec_path variable. */
    if (NULL != my_globals.path) {
        asprintf(&value, "OMPI_exec_path=%s", my_globals.path);
        opal_argv_append_nosize(&app->env, value);
        free(value);
    }

    /* Did the user request a specific wdir? */
    if (NULL != my_globals.wdir) {
        /* if this is a relative path, convert it to an absolute path */
        if (opal_path_is_absolute(my_globals.wdir)) {
            app->cwd = strdup(my_globals.wdir);
        } else {
            /* get the cwd */
            if (OPAL_SUCCESS != (rc = opal_getcwd(cwd, sizeof(cwd)))) {
                orte_show_help("help-orterun.txt", "orterun:init-failure",
                               true, "get the cwd", rc);
                goto cleanup;
            }
            /* construct the absolute path */
            app->cwd = opal_os_path(false, cwd, my_globals.wdir, NULL);
        }
        app->user_specified_cwd = true;
    } else {
        if (OPAL_SUCCESS != (rc = opal_getcwd(cwd, sizeof(cwd)))) {
            orte_show_help("help-orterun.txt", "orterun:init-failure",
                           true, "get the cwd", rc);
            goto cleanup;
        }
        app->cwd = strdup(cwd);
        app->user_specified_cwd = false;
    }

    /* Did the user specify a hostfile. Need to check for both 
     * hostfile and machine file. 
     * We can only deal with one hostfile per app context, otherwise give an error.
     */
    if (0 < (j = opal_cmd_line_get_ninsts(&cmd_line, "hostfile"))) {
        if(1 < j) {
            orte_show_help("help-orterun.txt", "orterun:multiple-hostfiles",
                           true, orte_basename, NULL);
            return ORTE_ERR_FATAL;
        } else {
            value = opal_cmd_line_get_param(&cmd_line, "hostfile", 0, 0);
            app->hostfile = strdup(value);
        }
    }
    if (0 < (j = opal_cmd_line_get_ninsts(&cmd_line, "machinefile"))) {
        if(1 < j || NULL != app->hostfile) {
            orte_show_help("help-orterun.txt", "orterun:multiple-hostfiles",
                           true, orte_basename, NULL);
            return ORTE_ERR_FATAL;
        } else {
            value = opal_cmd_line_get_param(&cmd_line, "machinefile", 0, 0);
            app->hostfile = strdup(value);
        }
    }
 
    /* Did the user specify any hosts? */
    if (0 < (j = opal_cmd_line_get_ninsts(&cmd_line, "host"))) {
        if (NULL != app->dash_host) {
            opal_argv_free(app->dash_host);
        }
        for (i = 0; i < j; ++i) {
            value = opal_cmd_line_get_param(&cmd_line, "host", i, 0);
            opal_argv_append_nosize(&app->dash_host, value);
        }
    }

    /* Get the numprocs */

    app->num_procs = (orte_std_cntr_t)my_globals.num_procs;

    /* If the user didn't specify the number of processes to run, then we
       default to launching an app process using every slot. We can't do
       anything about that here - we leave it to the RMAPS framework's
       components to note this and deal with it later.
        
       HOWEVER, we ONLY support this mode of operation if the number of
       app_contexts is equal to ONE. If the user provides multiple applications,
       we simply must have more information - in this case, generate an
       error.
    */
    if (app->num_procs == 0) {
        have_zero_np = true;  /** flag that we have a zero_np situation */
    }

    if (0 < total_num_apps && have_zero_np) {
        /** we have more than one app and a zero_np - that's no good.
         * note that we have to do this as a two step logic check since
         * the user may fail to specify num_procs for the first app, but
         * then give us another application.
         */
        orte_show_help("help-orterun.txt", "orterun:multi-apps-and-zero-np",
                       true, orte_basename, NULL);
        return ORTE_ERR_FATAL;
    }
    
    total_num_apps++;

    /* Do not try to find argv[0] here -- the starter is responsible
       for that because it may not be relevant to try to find it on
       the node where orterun is executing.  So just strdup() argv[0]
       into app. */

    app->app = strdup(app->argv[0]);
    if (NULL == app->app) {
        orte_show_help("help-orterun.txt", "orterun:call-failed",
                       true, orte_basename, "library", "strdup returned NULL", errno);
        rc = ORTE_ERR_NOT_FOUND;
        goto cleanup;
    }

    *app_ptr = app;
    app = NULL;
    *made_app = true;

    /* All done */

 cleanup:
    if (NULL != app) {
        OBJ_RELEASE(app);
    }
    if (cmd_line_made) {
        OBJ_DESTRUCT(&cmd_line);
    }
    return rc;
}


static int parse_appfile(opal_buffer_t *buf)
{
    size_t i, len;
    FILE *fp;
    char line[BUFSIZ];
    int rc, argc, app_num;
    char **argv;
    orte_app_context_t *app;
    bool blank, made_app;
    char bogus[] = "bogus ";
    char **tmp_env;
    orte_job_t *jdata = NULL;
    char *filename;
    char ***env;

    /* Try to open the file */
    fp = fopen(my_globals.appfile, "r");
    if (NULL == fp) {
        orte_show_help("help-orterun.txt", "orterun:appfile-not-found", true,
                       my_globals.appfile);
        return ORTE_ERR_NOT_FOUND;
    }

    /* Read in line by line */

    line[sizeof(line) - 1] = '\0';
    app_num = 0;
    do {

        /* We need a bogus argv[0] (because when argv comes in from
           the command line, argv[0] is "orterun", so the parsing
           logic ignores it).  So create one here rather than making
           an argv and then pre-pending a new argv[0] (which would be
           rather inefficient). */

        line[0] = '\0';
        strcat(line, bogus);

        if (NULL == fgets(line + sizeof(bogus) - 1,
                          sizeof(line) - sizeof(bogus) - 1, fp)) {
            break;
        }

        /* Remove a trailing newline */

        len = strlen(line);
        if (len > 0 && '\n' == line[len - 1]) {
            line[len - 1] = '\0';
            if (len > 0) {
                --len;
            }
        }

        /* Remove comments */

        for (i = 0; i < len; ++i) {
            if ('#' == line[i]) {
                line[i] = '\0';
                break;
            } else if (i + 1 < len && '/' == line[i] && '/' == line[i + 1]) {
                line[i] = '\0';
                break;
            }
        }

        /* Is this a blank line? */

        len = strlen(line);
        for (blank = true, i = sizeof(bogus); i < len; ++i) {
            if (!isspace(line[i])) {
                blank = false;
                break;
            }
        }
        if (blank) {
            continue;
        }

        /* We got a line with *something* on it.  So process it */

        argv = opal_argv_split(line, ' ');
        argc = opal_argv_count(argv);
        if (argc > 0) {
            if (0 == strcmp(argv[1], "app-grp:")) {
                /* if the jdata is already active, then we need to pack
                 * it before creating the next
                 */
                if (NULL != jdata) {
                    if (ORCM_SUCCESS != (rc = opal_dss.pack(buf, &jdata, 1, ORTE_JOB))) {
                        ORTE_ERROR_LOG(rc);
                        OBJ_RELEASE(jdata);
                        return rc;
                    }
                    jdata = NULL;
                    app_num = 0;
                }
                /* setup the job object */
                jdata = OBJ_NEW(orte_job_t);
                if (2 < argc && NULL != argv[2]) {
                    jdata->name = strdup(argv[2]);
                }
                if (3 < argc && NULL != argv[3]) {
                    jdata->instance = strdup(argv[3]);
                }
            } else {
                tmp_env = NULL;
                if (NULL == jdata) {
                    /* may not have provided an app-grp */
                    jdata = OBJ_NEW(orte_job_t);
                }
                rc = create_app(argc, argv, &app, &made_app, &tmp_env, jdata);
                if (ORTE_SUCCESS != rc) {
                    /* Assume that the error message has already been
                       printed; no need to cleanup -- we can just exit */
                    if (NULL == jdata) {
                        /* may not have provided an app-grp */
                        jdata = OBJ_NEW(orte_job_t);
                    }
                }
                if (NULL != tmp_env) {
                    opal_argv_free(tmp_env);
                }
                if (made_app) {
                    app->idx = app_num;
                    ++app_num;
                    opal_pointer_array_add(jdata->apps, app);
                    ++jdata->num_apps;
                }
            }
        }
    } while (!feof(fp));
    fclose(fp);

    /* All done */

    if (NULL != jdata) {
        if (ORCM_SUCCESS != (rc = opal_dss.pack(buf, &jdata, 1, ORTE_JOB))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(jdata);
            return rc;
        }
    }
    return ORTE_SUCCESS;
}

static int capture_cmd_line_params(int argc, int start, char **argv, char***app_env)
{
    int i, j, k;
    bool ignore;
    char *no_dups[] = {
        "grpcomm",
        "odls",
        "rml",
        "routed",
        NULL
    };
    char *tmp, **env = *app_env;
    
    for (i = 0; i < (argc-start); ++i) {
        if (0 == strcmp("-mca",  argv[i]) ||
            0 == strcmp("--mca", argv[i]) ) {
            /* It would be nice to avoid increasing the length
             * of the orted cmd line by removing any non-ORTE
             * params. However, this raises a problem since
             * there could be OPAL directives that we really
             * -do- want the orted to see - it's only the OMPI
             * related directives we could ignore. This becomes
             * a very complicated procedure, however, since
             * the OMPI mca params are not cleanly separated - so
             * filtering them out is nearly impossible.
             *
             * see if this is already present so we at least can
             * avoid growing the cmd line with duplicates
             */
            ignore = false;
            if (NULL != *app_env) {
                for (j=0; NULL != env[j]; j++) {
                    if (0 == strcmp(argv[i+1], env[j])) {
                        /* already here - if the value is the same,
                         * we can quitely ignore the fact that they
                         * provide it more than once. However, some
                         * frameworks are known to have problems if the
                         * value is different. We don't have a good way
                         * to know this, but we at least make a crude
                         * attempt here to protect ourselves.
                         */
                        if (0 == strcmp(argv[i+2], env[j+1])) {
                            /* values are the same */
                            ignore = true;
                            break;
                        } else {
                            /* values are different - see if this is a problem */
                            for (k=0; NULL != no_dups[k]; k++) {
                                if (0 == strcmp(no_dups[k], argv[i+1])) {
                                    /* print help message
                                     * and abort as we cannot know which one is correct
                                     */
                                    orte_show_help("help-orterun.txt", "orterun:conflicting-params",
                                                   true, "cfgi-file", argv[i+1],
                                                   argv[i+2], env[j+1]);
                                    return ORTE_ERR_BAD_PARAM;
                                }
                            }
                            /* this passed muster - just ignore it */
                            ignore = true;
                            break;
                        }
                    }
                }
            }
            if (!ignore) {
                asprintf(&tmp, "OMPI_MCA_%s", argv[i+1]);
                opal_setenv(tmp, argv[i+2], true, app_env);
                free(tmp);
            }
            i += 2;
        }
    }
    
    return ORTE_SUCCESS;
}

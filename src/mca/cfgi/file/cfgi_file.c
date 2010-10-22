/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config_private.h"
#include "include/constants.h"

#include "opal/dss/dss.h"
#include "opal/util/output.h"
#include "opal/util/argv.h"
#include "opal/util/os_path.h"
#include "opal/util/path.h"
#include "opal/util/opal_environ.h"

#include "orte/mca/rmcast/rmcast.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"

#include "mca/clip/clip.h"

#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/base/private.h"
#include "mca/cfgi/base/public.h"
#include "mca/cfgi/file/cfgi_file.h"

/*****************************************
 * Global Vars for appfile Arguments
 *****************************************/
static struct {
    int num_procs;
    char *wdir;
    char *path;
    char *hosts;
    bool constrained;
    bool gdb;
    int local_restarts;
    int global_restarts;
    bool continuous;
} my_globals;

opal_cmd_line_init_t cmd_line_opts[] = {
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
    
    /* End of list */
    { NULL, NULL, NULL, 
      '\0', NULL, NULL, 
      0,
      NULL, OPAL_CMD_LINE_TYPE_NULL,
      NULL }
};

/* API functions */

static int file_init(void);
static int file_finalize(void);

/* The module struct */

orcm_cfgi_base_module_t orcm_cfgi_file_module = {
    file_init,
    file_finalize
};

/* local functions */
static int create_app(int argc, char* argv[], orte_app_context_t **app_ptr,
                      bool *made_app, char ***app_env, orte_job_t *jdata);
static int capture_cmd_line_params(int argc, int start, char **argv, char***app_env);

/* local globals */
static bool have_zero_np = false;
static int total_num_apps = 0;

/* file will contain a set of key-value pairs:
 * app-grp: name instance
 *     applications to execute, one per line
 *
 * ends either at end-of-file or next app-grp. empty
 * lines are ignored. Each app-grp is launched as
 * a separate job
 */
static int file_init(void)
{
    size_t i, len;
    FILE *fp;
    char line[BUFSIZ];
    int rc, argc, app_num=0;
    char **argv;
    orte_app_context_t *app;
    bool blank, made_app;
    char bogus[] = "bogus ";
    char **tmp_env;
    orte_job_t *jdata=NULL;

    /* initialize the globals */
    my_globals.num_procs = 0;
    my_globals.wdir = NULL;
    my_globals.path = NULL;
    my_globals.hosts = NULL;
    my_globals.constrained = false;
    my_globals.gdb = false;
    my_globals.local_restarts = -1;
    my_globals.global_restarts = -1;
    my_globals.continuous = false;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output, "cfgi:file initialized to read %s",
                         mca_orcm_cfgi_file_component.file));

    /* Try to open the file */
    fp = fopen(mca_orcm_cfgi_file_component.file, "r");
    if (NULL == fp) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        return ORTE_ERR_NOT_FOUND;
    }

    /* wait for any existing action to complete */
    OPAL_ACQUIRE_THREAD(&orcm_cfgi_base.lock, &orcm_cfgi_base.cond, &orcm_cfgi_base.active);

    /* Read in line by line */
    line[sizeof(line) - 1] = '\0';
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
        OPAL_OUTPUT_VERBOSE((3, orcm_cfgi_base.output,
                             "READ LINE %s", line));

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
                /* if the jdata is already active, then we need to spawn
                 * it before creating the next
                 */
                if (NULL != jdata) {
                    if (ORCM_SUCCESS != (rc = orcm_cfgi_base_spawn_app(jdata))) {
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
                    OBJ_RELEASE(jdata);
                    return rc;
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

    if (NULL != jdata) {
        if (ORCM_SUCCESS != (rc = orcm_cfgi_base_spawn_app(jdata))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(jdata);
        }
    }

    /* release the thread */
    OPAL_RELEASE_THREAD(&orcm_cfgi_base.lock, &orcm_cfgi_base.cond, &orcm_cfgi_base.active);
    return rc;
}

static int file_finalize(void)
{
    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output, "cfgi:file finalized"));
    return ORCM_SUCCESS;
}

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
    
    /*
     * Get mca parameters so we can pass them to the app.
     * Use the count determined above to make sure we do not go past
     * the executable name. Example:
     *   -np 2 -mca foo bar ./my-app
     * We want to pick up '-mca foo bar' but not '-mca bip bop' as
     * that is intended as an arg for my-app
     */
    if (ORTE_SUCCESS != (rc = capture_cmd_line_params(argc, count, argv, &app->env))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
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


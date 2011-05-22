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

#include <stdio.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <stdarg.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif  /* HAVE_DIRENT_H */
#ifdef HAVE_SYS_INOTIFY_H
#include <sys/inotify.h>
#endif

#include "opal/class/opal_pointer_array.h"
#include "opal/dss/dss.h"
#include "opal/util/output.h"
#include "opal/util/argv.h"
#include "opal/util/basename.h"
#include "opal/util/os_path.h"
#include "opal/util/os_dirpath.h"
#include "opal/util/path.h"
#include "opal/util/opal_environ.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"
#include "orte/util/error_strings.h"
#include "orte/util/name_fns.h"

#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/base/private.h"
#include "mca/cfgi/base/public.h"
#include "mca/cfgi/file/cfgi_file_lex.h"
#include "mca/cfgi/file/cfgi_file.h"

#define ORCM_CFGI_FILE_MAX_LINE_LENGTH 1024

/* API functions */

static int file_init(void);
static int file_finalize(void);
static void activate(void);

/* The module struct */

orcm_cfgi_base_module_t orcm_cfgi_file_module = {
    file_init,
    file_finalize,
    activate
};

/* local globals */
static bool initialized = false;
static bool enabled = false;
static opal_event_t *probe_ev=NULL;
static struct timeval probe_time;
static bool timer_in_use=false;
#ifdef HAVE_SYS_INOTIFY_H
static int notifier = -1;
static int watch;
#endif

/* local functions */
static void check_config(int fd, short args, void *cbdata);
static int parse_file(char *filename,
                      opal_pointer_array_t *apps);
static void check_installed(bool check_all);
#ifdef HAVE_SYS_INOTIFY_H
static void inotify_handler(int fd, short args, void *cbdata);
#endif

static int file_init(void)
{
    if (initialized) {
        return ORCM_SUCCESS;
    }
    initialized = true;

#ifdef HAVE_SYS_INOTIFY_H
    /* init the inotifier system */
    notifier = inotify_init();
#else
    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                         "cfgi:file initialized to check dir %s every %d seconds",
                         mca_orcm_cfgi_file_component.dir,
                         mca_orcm_cfgi_file_component.rate));
#endif

    return ORCM_SUCCESS;
}


static void activate(void)
{
    int rc;
    DIR *dirp;

    /* take control */
    ORTE_ACQUIRE_THREAD(&orcm_cfgi_base.ctl);

    if (enabled) {
        /* we get reentered when daemons reappear so that
         * any pending jobs can be started
         */
        check_installed(true);
        /* release control */
        ORTE_RELEASE_THREAD(&orcm_cfgi_base.ctl);
        return;
    }
    enabled = true;

    /* check for existence of the directory. If it doesn't yet
     * exist, then we have to use the timer until it shows up
     */
    if (NULL == (dirp = opendir(mca_orcm_cfgi_file_component.dir))) {
        if (0 < opal_output_get_verbosity(orcm_cfgi_base.output)) {
            orte_show_help("help-cfgi-file.txt", "no-dir",
                           true, mca_orcm_cfgi_file_component.dir);
        }
        timer_in_use = true;
        goto fallback;
    }

#ifdef HAVE_SYS_INOTIFY_H
    /* setup to watch the config dir - CREATE always is followed by
     * a MODIFY event, so don't need both
     */
    if (0 > (watch = inotify_add_watch(notifier, mca_orcm_cfgi_file_component.dir,
                                           IN_DELETE | IN_MODIFY | IN_MOVE))) {
        /* error */
        close(notifier);
        goto fallback;
    }
    /* start the watcher event */
    probe_ev = (opal_event_t*)malloc(sizeof(opal_event_t));
    opal_event_set(opal_event_base, probe_ev, notifier,
                   OPAL_EV_READ|OPAL_EV_PERSIST, inotify_handler, NULL);
    timer_in_use = false;
    ORTE_RELEASE_THREAD(&orcm_cfgi_base.ctl);
    /* process it the first time */
    check_config(0, 0, NULL);
    return;
#endif

 fallback:

    /* setup the probe timer */
    if (0 <  mca_orcm_cfgi_file_component.rate) {
        probe_time.tv_sec = mca_orcm_cfgi_file_component.rate;
        probe_time.tv_usec = 0;
        probe_ev = (opal_event_t*)malloc(sizeof(opal_event_t));
        opal_event_evtimer_set(opal_event_base, probe_ev, check_config, NULL);
        timer_in_use = true;
        /* process it the first time */
        ORTE_RELEASE_THREAD(&orcm_cfgi_base.ctl);
        check_config(0, 0, NULL);
        return;
    }

    opal_output(0, "%s CANNOT ACTIVATE INSTALL CONFIG MONITORING",
                   ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    enabled = false;
    ORTE_RELEASE_THREAD(&orcm_cfgi_base.ctl);
}

static int file_finalize(void)
{
    int i;

    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output, "cfgi:file finalized"));

    if (NULL != probe_ev) {
        opal_event_del(probe_ev);
        free(probe_ev);
        probe_ev = NULL;
    }

#ifdef HAVE_SYS_INOTIFIER_H
    if (0 <= notifier) {
        close(notifier);
    }
#endif

    return ORCM_SUCCESS;
}

static void link_launch(orcm_cfgi_app_t *app,
                        orcm_cfgi_run_t *run,
                        bool linkall)
{
    orcm_cfgi_caddy_t *caddy;
    int j, k;
    orcm_cfgi_exec_t *exec, *eptr;
    orcm_cfgi_version_t *vers, *vptr;
    orcm_cfgi_bin_t *bin;
    orte_job_t *jdat, *jptr;
    orte_app_context_t *ax;
    bool found;

    /* link all the required binaries */
    found = false;
    for (j=0; j < run->binaries.size; j++) {
        if (NULL == (bin = (orcm_cfgi_bin_t*)opal_pointer_array_get_item(&run->binaries, j))) {
            continue;
        }
        /* find the matching executable */
        exec = NULL;
        for (k=0; k < app->executables.size; k++) {
            if (NULL == (eptr = (orcm_cfgi_exec_t*)opal_pointer_array_get_item(&app->executables, k))) {
                continue;
            }
            if (0 == strcmp(eptr->appname, bin->appname)) {
                exec = eptr;
                break;
            }
        }
        /* if not found, then skip - hasn't been defined yet */
        if (NULL == exec) {
            continue;
        }
        /* find the matching version */
        vers = NULL;
        /* if not found, then skip - hasn't been defined yet */
        for (k=0; k < exec->versions.size; k++) {
            if (NULL == (vptr = (orcm_cfgi_version_t*)opal_pointer_array_get_item(&exec->versions, k))) {
                continue;
            }
            if (0 == strcmp(vptr->version, bin->version)) {
                vers = vptr;
                break;
            }
        }
        /* if not found, then skip - hasn't been defined yet */
        if (NULL == vers) {
            continue;
        }
        /* have we already matched this one */
        if (NULL != bin->vers) {
            if (linkall) {
                /* attempt to launch all */
                found = true;
            }
            continue;
        }
        /* is there enough room left for this number of procs? */
        if (0 <= exec->process_limit) {
            if (exec->process_limit < (bin->num_procs + exec->total_procs)) {
                opal_output(0, "%s EXECUTABLE %s: MAX NUMBER OF ALLOWED PROCS (%d) EXCEEDED - CANNOT ADD %d PROCS, ALREADY HAVE %d",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            (NULL == exec->appname) ? "NULL" : exec->appname,
                            exec->process_limit, bin->num_procs, exec->total_procs);
                continue;
            }
        }
        /* make the link */
        bin->vers = vers;
        bin->vers_idx = opal_pointer_array_add(&vers->binaries, bin);
        bin->exec = exec;
        found = true;
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s LINKED BINARY %s TO VERSION %s:%s with num_procs %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             bin->binary, exec->appname, vers->version, bin->num_procs));
    }
    /* if we found at least one binary, launch it */
    if (found) {
        /* need to create the ORTE job object for this job */
        jdat = OBJ_NEW(orte_job_t);
        jdat->name = strdup(run->application);
        jdat->instance = strdup(run->instance);
        for (j=0; j < run->binaries.size; j++) {
            if (NULL == (bin = (orcm_cfgi_bin_t*)opal_pointer_array_get_item(&run->binaries, j))) {
                continue;
            }
            if (NULL == bin->vers) {
                /* not installed yet */
                continue;
            }
            /* create an app_context for this binary */
            ax = OBJ_NEW(orte_app_context_t);
            ax->app = strdup(bin->binary);
            /* copy the argv across */
            if (NULL != bin->vers->argv) {
                ax->argv = opal_argv_copy(bin->vers->argv);
            }
            /* stick the command at the beginning of the argv */
            opal_argv_prepend_nosize(&ax->argv, bin->binary);
            /* set num procs */
            ax->num_procs = bin->num_procs;
            /* add it to the job */
            ax->idx = opal_pointer_array_add(jdat->apps, ax);
            jdat->num_apps++;
        }
        /* notify the launcher */
        caddy = OBJ_NEW(orcm_cfgi_caddy_t);
        caddy->cmd = ORCM_CFGI_SPAWN;
        caddy->jdata = jdat;
        opal_fd_write(orcm_cfgi_base.launch_pipe[1], sizeof(orcm_cfgi_caddy_t*), &caddy);
    }
}

static void check_installed(bool check_all)
{
    int i, n;
    orcm_cfgi_app_t *app;
    orcm_cfgi_run_t *run;

    /* run a check of the installed apps against
     * the configured apps so we can start anything that was awaiting
     * installation
     */
    for (i=0; i < orcm_cfgi_base.installed_apps.size; i++) {
        if (NULL == (app = (orcm_cfgi_app_t*)opal_pointer_array_get_item(&orcm_cfgi_base.installed_apps, i))) {
            continue;
        }
        if (!check_all && !app->modified) {
            OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                 "APP %s HAS NOT BEEN MODIFIED",
                                 app->application));
            continue;
        }
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "CHECKING INSTALL-RUNNING CONFIG FOR APP %s", app->application));
        /* reset the flag */
        app->modified = false;
        /* search the configuration array for instances of this app */
        for (n=0; n < orcm_cfgi_base.confgd_apps.size; n++) {
            if (NULL == (run = (orcm_cfgi_run_t*)opal_pointer_array_get_item(&orcm_cfgi_base.confgd_apps, n))) {
                continue;
            }
            if (NULL == run->app) {
                /* still waiting for app to be defined - is this it? */
                if (0 == strcmp(run->application, app->application)) {
                    /* yep - see if we can run it */
                    if (0 <= app->max_instances && app->max_instances <= app->num_instances) {
                        /* at our limit - can't run at this time */
                        continue;
                    }
                    /* add this instance */
                    run->app = app;
                    run->app_idx = opal_pointer_array_add(&app->instances, run);
                    app->num_instances++;
                    link_launch(app, run, check_all);
                }
            } else if (0 == strcmp(run->application, app->application)) {
                OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                     "%s EXISTING INSTANCE %s:%s CAN BE LAUNCHED",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     run->application, run->instance));
                /* check to see what has changed, and launch it if reqd */
                link_launch(app, run, check_all);
            }
        }
    }
}

static void check_config(int fd, short args, void *cbdata)
{
    DIR *dirp = NULL;
    struct dirent * dir_entry;
    struct stat buf;
    int i, rc, n, j, k, m;
    char *fullpath;
    orcm_cfgi_app_t *app, *app2, *aptr;
    orcm_cfgi_run_t *run;
    orcm_cfgi_exec_t *exec, *exec2, *eptr;
    orcm_cfgi_version_t *vers, *vers2, *vptr;
    orcm_cfgi_bin_t *bin;
    orte_job_t *jdat, *jptr;
    orte_app_context_t *ax;
    opal_pointer_array_t found_apps;
    bool found, dir_found;
    orcm_cfgi_caddy_t *caddy;

    /* take control */
    ORTE_ACQUIRE_THREAD(&orcm_cfgi_base.ctl);

    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                         "%s CHECKING CONFIG DIRECTORY %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         mca_orcm_cfgi_file_component.dir));

    /* Open the directory so we can get a listing */
    if (NULL == (dirp = opendir(mca_orcm_cfgi_file_component.dir))) {
        if (0 < opal_output_get_verbosity(orcm_cfgi_base.output)) {
            orte_show_help("help-cfgi-file.txt", "no-dir",
                           true, mca_orcm_cfgi_file_component.dir);
        }
        dir_found = false;
        goto restart;
    }
    dir_found = true;

    /* setup the array of apps */
    OBJ_CONSTRUCT(&found_apps, opal_pointer_array_t);
    opal_pointer_array_init(&found_apps, 16, INT_MAX, 16);

    /* cycle thru the directory */
    while (NULL != (dir_entry = readdir(dirp))) {
        /* Skip the obvious */
        if (0 == strncmp(dir_entry->d_name, ".", strlen(".")) ||
            0 == strncmp(dir_entry->d_name, "..", strlen(".."))) {
            continue;
        }
        /* Skip editor-related files */
        if (NULL != strstr(dir_entry->d_name, ".swp") ||
            NULL != strstr(dir_entry->d_name, ".swx") ||
            NULL != strchr(dir_entry->d_name, '~')) {
            continue;
        }
        if ('#' == dir_entry->d_name[0]) {
            continue;
        }

        /* parse the file, adding all found apps to the array */
        fullpath = opal_os_path(false, mca_orcm_cfgi_file_component.dir, dir_entry->d_name, NULL);
        if (ORCM_SUCCESS != (rc = parse_file(fullpath, &found_apps))) {
            OPAL_OUTPUT_VERBOSE((1, orcm_cfgi_base.output,
                                 "%s CANNOT PARSE FILE %s: %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 dir_entry->d_name, ORTE_ERROR_NAME(rc)));
        }
        free(fullpath);
    }
    closedir(dirp);

    /* cycle thru the installed apps */
    for (i=0; i < orcm_cfgi_base.installed_apps.size; i++) {
        if (NULL == (app = (orcm_cfgi_app_t*)opal_pointer_array_get_item(&orcm_cfgi_base.installed_apps, i))) {
            continue;
        }
        app->modified = false;
        /* is this app present in the found apps? */
        app2 = NULL;
        for (j=0; j < found_apps.size; j++) {
            if (NULL == (aptr = (orcm_cfgi_app_t*)opal_pointer_array_get_item(&found_apps, j))) {
                continue;
            }
            if (0 == strcmp(app->application, aptr->application)) {
                app2 = aptr;
                /* remove it from the found_apps array as we will now process it */
                opal_pointer_array_set_item(&found_apps, j, NULL);
                break;
            }
        }
        if (NULL == app2) {
            OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                 "%s APP %s IS NO LONGER INSTALLED",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 app->application));
            /* no longer present - remove this object from the installed array */
            opal_pointer_array_set_item(&orcm_cfgi_base.installed_apps, app->idx, NULL);
            /* find all instances */
            for (j=0; j < app->instances.size; j++) {
                if (NULL == (run = (orcm_cfgi_run_t*)opal_pointer_array_get_item(&app->instances, j))) {
                    continue;
                }
                OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                     "%s APP %s IS NO LONGER INSTALLED - KILLING INSTANCE %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     app->application, run->instance));
                /* remove it from the array */
                opal_pointer_array_set_item(&app->instances, j, NULL);
                run->app = NULL;
                run->app_idx = -1;
                /* delink all the binaries */
                for (k=0; k < run->binaries.size; k++) {
                    if (NULL == (bin = (orcm_cfgi_bin_t*)opal_pointer_array_get_item(&run->binaries, k))) {
                        continue;
                    }
                    bin->vers = NULL;
                    bin->exec = NULL;
                }
                /* kill the associated executing job, if any */
                caddy = OBJ_NEW(orcm_cfgi_caddy_t);
                caddy->cmd = ORCM_CFGI_KILL_JOB;
                /* retain the run object as it has -not- been removed from
                 * the running config
                 */
                OBJ_RETAIN(run);
                caddy->run = run;
                /* send it off to be processed */
                opal_fd_write(orcm_cfgi_base.launch_pipe[1], sizeof(orcm_cfgi_caddy_t*), &caddy);
            }
            /* release it */
            OBJ_RELEASE(app);
            continue;
        }
        /* app was present - did we modify it */
        if (app->max_instances != app2->max_instances) {
            app->max_instances = app2->max_instances;
            app->modified = true;
        }
        /* did we remove any executables? */
        for (j=0; j < app->executables.size; j++) {
            if (NULL == (exec = (orcm_cfgi_exec_t*)opal_pointer_array_get_item(&app->executables, j))) {
                continue;
            }
            /* is it present in the found apps */
            exec2 = NULL;
            for (k=0; k < app2->executables.size; k++) {
                if (NULL == (eptr = (orcm_cfgi_exec_t*)opal_pointer_array_get_item(&app2->executables, k))) {
                    continue;
                }
                if (0 == strcmp(exec->appname, eptr->appname)) {
                    exec2 = eptr;
                    break;
                }
            }
            if (NULL == exec2) {
                OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                     "%s APP %s EXECUTABLE %s IS NO LONGER INSTALLED",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     app->application, exec->appname));
                /* this executable has been removed */
                opal_pointer_array_set_item(&app->executables, j, NULL);
                /* find all instances
                 * that use this executable and kill associated binaries
                 */
                for (k=0; k < app->instances.size; k++) {
                    if (NULL == (run = (orcm_cfgi_run_t*)opal_pointer_array_get_item(&app->instances, k))) {
                        continue;
                    }
                    /* search the binaries to see if they include this executable */
                    for (n=0; n < run->binaries.size; n++) {
                        if (NULL == (bin = (orcm_cfgi_bin_t*)opal_pointer_array_get_item(&run->binaries, n))) {
                            continue;
                        }
                        if (0 == strcmp(bin->appname, exec->appname)) {
                            OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                                 "%s APP %s EXECUTABLE %s IS NO LONGER INSTALLED - KILLING BINARY %s",
                                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                                 app->application, exec->appname, bin->binary));
                            exec->total_procs -= bin->num_procs;
                            /* ensure we know it is no longer pointing to an installed exec/version */
                            bin->vers = NULL;
                            bin->exec = NULL;
                            /* kill the associated executing exec, if any */
                            caddy = OBJ_NEW(orcm_cfgi_caddy_t);
                            caddy->cmd = ORCM_CFGI_KILL_EXE;
                            /* retain the run object as it has -not- been removed from
                             * the running config
                             */
                            OBJ_RETAIN(run);
                            caddy->run = run;
                            /* send it off to be processed */
                            opal_fd_write(orcm_cfgi_base.launch_pipe[1], sizeof(orcm_cfgi_caddy_t*), &caddy);
                            break;
                        }
                    }
                }
                OBJ_RELEASE(exec);
                continue;
            }
            /* kept the exec, but was it modified */
            if (exec->process_limit != exec2->process_limit) {
                exec->process_limit = exec2->process_limit;
                app->modified = true;
            }
            /* did we remove any versions */
            for (k=0; k < exec->versions.size; k++) {
                if (NULL == (vers = (orcm_cfgi_version_t*)opal_pointer_array_get_item(&exec->versions, k))) {
                    continue;
                }
                /* is it present in the found app/exec */
                vers2 = NULL;
                for (n=0; n < exec2->versions.size; n++) {
                    if (NULL == (vptr = (orcm_cfgi_version_t*)opal_pointer_array_get_item(&exec2->versions, n))) {
                        continue;
                    }
                    if (0 == strcmp(vptr->version, vers->version)) {
                        vers2 = vptr;
                        /* since we have this version, we can remove it from
                         * the found app
                         */
                        opal_pointer_array_set_item(&exec2->versions, n, NULL);
                        break;
                    }
                }
                if (NULL != vers2) {
                    OBJ_RELEASE(vers2);
                    continue;
                }
                OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                     "%s APP %s EXEC %s VERSION %s IS NO LONGER INSTALLED",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     app->application, exec->appname, vers->version));
                /* nope - been removed, so take it out of the array */
                opal_pointer_array_set_item(&exec->versions, k, NULL);
                /* find all instances and kill this version */
                for (m=0; m < app->instances.size; m++) {
                    if (NULL == (run = (orcm_cfgi_run_t*)opal_pointer_array_get_item(&app->instances, m))) {
                        continue;
                    }
                    /* search the binaries to see if they include this version */
                    for (n=0; n < run->binaries.size; n++) {
                        if (NULL == (bin = (orcm_cfgi_bin_t*)opal_pointer_array_get_item(&run->binaries, n))) {
                            continue;
                        }
                        if (0 == strcmp(bin->appname, exec->appname) &&
                            0 == strcmp(bin->version, vers->version)) {
                            OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                                 "%s APP %s EXECUTABLE %s VERSION %s IS NO LONGER INSTALLED - KILLING BINARY %s",
                                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                                 app->application, exec->appname, vers->version, bin->binary));
                            exec->total_procs -= bin->num_procs;
                            /* ensure we know it is no longer pointing to an installed version */
                            bin->vers = NULL;
                            /* kill the associated executing exec, if any */
                            caddy = OBJ_NEW(orcm_cfgi_caddy_t);
                            caddy->cmd = ORCM_CFGI_KILL_EXE;
                            /* retain the run object as it has -not- been removed from
                             * the running config
                             */
                            OBJ_RETAIN(run);
                            caddy->run = run;
                            /* send it off to be processed */
                            opal_fd_write(orcm_cfgi_base.launch_pipe[1], sizeof(orcm_cfgi_caddy_t*), &caddy);
                            break;
                        }
                    }
                }
                /* cleanup */
                OBJ_RELEASE(vers);
            }
        }
        /* did we add any executables or versions */
        for (k=0; k < app2->executables.size; k++) {
            if (NULL == (exec2 = (orcm_cfgi_exec_t*)opal_pointer_array_get_item(&app2->executables, k))) {
                continue;
            }
            exec = NULL;
            for (j=0; j < app->executables.size; j++) {
                if (NULL == (eptr = (orcm_cfgi_exec_t*)opal_pointer_array_get_item(&app->executables, j))) {
                    continue;
                }
                if (0 == strcmp(eptr->appname, exec2->appname)) {
                    exec = eptr;
                    break;
                }
            }
            if (NULL == exec) {
                OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                     "%s APP %s ADDING EXECUTABLE %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     app->application, exec2->appname));
                /* added this exec - just move it across */
                opal_pointer_array_set_item(&app2->executables, k, NULL);
                exec2->idx = opal_pointer_array_add(&app->executables, exec2);
                app->modified = true;
                continue;
            }
            /* exec already present, and we dealt with mods above - so
             * see if any versions were added.
             */
            for (j=0; j < exec2->versions.size; j++) {
                if (NULL == (vers = (orcm_cfgi_version_t*)opal_pointer_array_get_item(&exec2->versions, j))) {
                    continue;
                }
                /* if already present, ignore */
                vers2 = NULL;
                for (n=0; n < exec->versions.size; n++) {
                    if (NULL == (vptr = (orcm_cfgi_version_t*)opal_pointer_array_get_item(&exec->versions, n))) {
                        continue;
                    }
                    if (0 == strcmp(vptr->version, vers->version)) {
                        vers2 = vptr;
                        break;
                    }
                }
                if (NULL == vers2) {
                    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                         "%s APP %s ADDING EXECUTABLE %s VERSION %s",
                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                         app->application, exec2->appname, vers->version));
                    opal_pointer_array_set_item(&exec2->versions, j, NULL);
                    vers->exec = exec;
                    vers->idx = opal_pointer_array_add(&exec->versions, vers);
                    app->modified = true;
                } else {
                    OBJ_RELEASE(vers2);
                }
            }
        }
        /* done with this entry */
        OBJ_RELEASE(app2);
    }

    /* any added applications get handled now - anything still in found_apps
     * would have been added
     */
    for (j=0; j < found_apps.size; j++) {
        if (NULL == (aptr = (orcm_cfgi_app_t*)opal_pointer_array_get_item(&found_apps, j))) {
            continue;
        }
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s ADDING APP %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             aptr->application));
        /* just shift the entry to the installed_apps array */
        aptr->idx = opal_pointer_array_add(&orcm_cfgi_base.installed_apps, aptr);
        /* mark it as modified so it will be handled below */
        aptr->modified = true;
    }
    OBJ_DESTRUCT(&found_apps);

    /* check installed vs configd for anything needing starting,
     * but only check modified apps
     */
    check_installed(false);

 restart:
#ifdef HAVE_SYS_INOTIFY_H
    if (dir_found) {
        if (timer_in_use) {
            /* redefine the event to use inotify now
             * that the dir has been found
             */
            if (0 > (watch = inotify_add_watch(notifier, mca_orcm_cfgi_file_component.dir,
                                               IN_DELETE | IN_MODIFY | IN_MOVE))) {
                close(notifier);
                opal_event_evtimer_add(probe_ev, &probe_time);
            } else {
                opal_event_del(probe_ev);
                opal_event_set(opal_event_base, probe_ev, notifier,
                               OPAL_EV_READ|OPAL_EV_PERSIST, inotify_handler, NULL);
                opal_event_add(probe_ev, 0);
                timer_in_use = false;
            }
        } else {
            /* reset the event */
            opal_event_add(probe_ev, 0);
        }
    } else {
        /* restart the timer so we keep looking for it */
        opal_event_evtimer_add(probe_ev, &probe_time);
    }
#else
    /* restart the timer */
    opal_event_evtimer_add(probe_ev, &probe_time);
#endif
    /* release control */
    ORTE_RELEASE_THREAD(&orcm_cfgi_base.ctl);
}


/**
 * Return the integer following an optional = (actually may only return positive ints)
 */
static int parse_int(void)
{
    int value;
    char *ptr;

    if (ORCM_CFGI_FILE_EQUAL == orcm_cfgi_file_lex()) {
        /* they included an = sign, so move to next token */
        if (ORCM_CFGI_FILE_STRING != orcm_cfgi_file_lex()) {
            return -1;
        }
    }
    value = strtol(orcm_cfgi_file_value.sval, &ptr, 10);
    if (NULL != ptr && 0 < strlen(ptr)) {
        return -1;
    }
    return value;
}


/**
 * Return the string following an optional =
 */
static char *parse_string(void)
{
    char *tmp;
    int i, m;

    if (ORCM_CFGI_FILE_EQUAL == orcm_cfgi_file_lex()){
        /* they included an = sign, so move to next token */
        if (ORCM_CFGI_FILE_STRING != orcm_cfgi_file_lex()) {
            return NULL;
        }
    }
    /* if there are quotes in here, remove them */
    if (NULL != strchr(orcm_cfgi_file_value.sval, '\"')) {
        tmp = (char*)calloc(strlen(orcm_cfgi_file_value.sval)+1, sizeof(char));
        for (i=0,m=0; i < strlen(orcm_cfgi_file_value.sval); i++) {
            if ('\"' != orcm_cfgi_file_value.sval[i]) {
                tmp[m] = orcm_cfgi_file_value.sval[i];
                m++;
            }
        }
        return tmp;
    }
    return strdup(orcm_cfgi_file_value.sval);
}


static int parse_file(char *filename,
                      opal_pointer_array_t *apps)
{
    char *cptr, *executable, *version, *argv, *tmp, *binary;
    int token, rc=ORCM_SUCCESS;
    int i, ival;
    orcm_cfgi_app_t *app=NULL, *aptr, *curapp;
    orcm_cfgi_exec_t *exec=NULL, *eptr;
    orcm_cfgi_version_t *vers=NULL, *vptr;
    bool found;
    struct stat buf;

    orcm_cfgi_file_in = fopen(filename, "r");
    if (NULL == orcm_cfgi_file_in) {
        opal_output(0, "%s UNABLE TO OPEN FILE %s",
                       ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), filename);
        return ORTE_ERR_FILE_OPEN_FAILURE;
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                         "%s PARSING CONFIGURATION FILE %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), filename));

    while (!orcm_cfgi_file_done) {
        token = orcm_cfgi_file_lex();

        switch (token) {
        case ORCM_CFGI_FILE_DONE:
            orcm_cfgi_file_done = true;
            if (NULL != app) {
                if (1 < opal_output_get_verbosity(orcm_cfgi_base.output)) {
                    opal_output(0, "READ INSTALL CONFIG:");
                    orcm_cfgi_base_dump(NULL, NULL, app, ORCM_CFGI_APP);
                }
                /* if the app hasn't been added yet */
                if (app->idx < 0) {
                    /* see if we already have a definition for this app */
                    found = false;
                    for (i=0; i < apps->size; i++) {
                        if (NULL == (aptr = (orcm_cfgi_app_t*)opal_pointer_array_get_item(apps, i))) {
                            continue;
                        }
                        if (0 == strcmp(app->application, aptr->application)) {
                            /* found app */
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        /* this is a new app - check for validity */
                        if (!orcm_cfgi_app_definition_valid(app)) {
                            opal_output(0, "%s APP %s NOT VALID",
                                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                        (NULL == app->application) ? "NULL" : app->application);
                            rc = ORTE_ERR_BAD_PARAM;
                            OBJ_RELEASE(app);
                            goto depart;
                        }
                        /* store it before leaving */
                        app->idx = opal_pointer_array_add(apps, app);
                    }
                }
            }
            break;

        case ORCM_CFGI_FILE_NEWLINE:
            break;

        case ORCM_CFGI_FILE_APPLICATION:
            if (NULL == (cptr = parse_string())) {
                orte_show_help("help-cfgi-file.txt", "no-app-name", true);
                rc = ORTE_ERR_BAD_PARAM;
                goto depart;
            }
            if (NULL == app) {
                /* start a new app - create record for it */
                app = OBJ_NEW(orcm_cfgi_app_t);
                app->application = strdup(cptr);
                exec = NULL;  /* start the exec over */
                vers = NULL;
            } else {
                /* are we already working this app */
                if (0 != strcmp(cptr, app->application)) {
                    /* this starts a different app */
                    if (1 < opal_output_get_verbosity(orcm_cfgi_base.output)) {
                        opal_output(0, "READ INSTALL CONFIG:");
                        orcm_cfgi_base_dump(NULL, NULL, app, ORCM_CFGI_APP);
                    }
                    /* check for validity */
                    if (!orcm_cfgi_app_definition_valid(app)) {
                        opal_output(0, "%s APP %s NOT VALID",
                                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                    (NULL == app->application) ? "NULL" : app->application);
                        rc = ORTE_ERR_BAD_PARAM;
                        OBJ_RELEASE(app);
                        free(cptr);
                        goto depart;
                    }
                    if (app->idx < 0) {
                        /* hasn't been added yet, so do so
                         * before we start on the new one
                         */
                        app->idx = opal_pointer_array_add(apps, app);
                    }
                    /* see if we already have a definition for this app */
                    found = false;
                    for (i=0; i < orcm_cfgi_base.installed_apps.size; i++) {
                        if (NULL == (aptr = (orcm_cfgi_app_t*)opal_pointer_array_get_item(apps, i))) {
                            continue;
                        }
                        if (0 == strcmp(cptr, aptr->application)) {
                            /* found app */
                            app = aptr;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        /* start a new app - create record for it */
                        app = OBJ_NEW(orcm_cfgi_app_t);
                        app->application = strdup(cptr);
                    }
                }
                exec = NULL;  /* start the exec over */
                vers = NULL;
            }
            free(cptr);
            break;

        case ORCM_CFGI_FILE_MAX_INSTANCES:
            /* if we don't have an active app, that's an error */
            if (NULL == app) {
                /* cannot process this */
                rc = ORTE_ERR_BAD_PARAM;
                goto depart;
            }
            if ((app->max_instances = parse_int()) < 0) {
                orte_show_help("help-cfgi-file.txt", "bad-max-instances", true,
                               filename, orcm_cfgi_file_line, token, 
                               orcm_cfgi_file_value.sval);
                OBJ_RELEASE(app);
                rc = ORTE_ERR_BAD_PARAM;
                goto depart;
            }
            break;

        case ORCM_CFGI_FILE_EXECUTABLE:
            /* if we don't have an active app, that's an error */
            if (NULL == app) {
                /* cannot process this */
                rc = ORTE_ERR_BAD_PARAM;
                goto depart;
            }
            if (NULL == (executable = parse_string())) {
                orte_show_help("help-cfgi-file.txt", "no-exec", true,
                               filename, orcm_cfgi_file_line, token,
                               orcm_cfgi_file_value.sval);
                OBJ_RELEASE(app);
                rc = ORTE_ERR_BAD_PARAM;
                goto depart;
            }
            /* do I already have an entry for this executable in this app? */
            exec = NULL;
            for (i=0; i < app->executables.size; i++) {
                if (NULL == (eptr = (orcm_cfgi_exec_t*)opal_pointer_array_get_item(&app->executables, i))) {
                    continue;
                }
                if (0 == strcmp(eptr->appname, executable)) {
                    exec = eptr;
                    break;
                }
            }
            if (NULL == exec) {
                /* nope - add this executable */
                exec = OBJ_NEW(orcm_cfgi_exec_t);
                exec->appname = strdup(executable);
                exec->idx = opal_pointer_array_add(&app->executables, exec);
            }
            free(executable);
            break;

        case ORCM_CFGI_FILE_VERSION:
            /* if we don't have an active job, that's an error */
            if (NULL == app) {
                /* cannot process this */
                rc = ORTE_ERR_BAD_PARAM;
                goto depart;
            }
            /* if we don't have an active exec, that's an error */
            if (NULL == exec) {
                /* cannot process this */
                OBJ_RELEASE(app);
                rc = ORTE_ERR_BAD_PARAM;
                goto depart;
            }
            if (NULL == (version = parse_string())) {
                orte_show_help("help-cfgi-file.txt", "no-version", true,
                               filename, orcm_cfgi_file_line, token,
                               orcm_cfgi_file_value.sval);
                OBJ_RELEASE(app);
                rc = ORTE_ERR_BAD_PARAM;
                goto depart;
            }
            /* do we already have a record of this version? */
            vers = NULL;
            for (i=0; i < exec->versions.size; i++) {
                if (NULL == (vptr = (orcm_cfgi_version_t*)opal_pointer_array_get_item(&exec->versions, i))) {
                    continue;
                }
                if (0 == strcmp(vptr->version, version)) {
                    vers = vptr;
                    break;
                }
            }
            if (NULL == vers) {
                /* nope - add it */
                vers = OBJ_NEW(orcm_cfgi_version_t);
                vers->exec = exec;
                vers->version = strdup(version);
                vers->idx = opal_pointer_array_add(&exec->versions, vers);
                /* get the modification time stamp for that binary */
                asprintf(&binary, "%s_%s", exec->appname, version);
                tmp = opal_path_findv(binary, X_OK, environ, NULL);
                if (NULL == tmp) {
                    /* binary wasn't found - just leave it */
                    free(binary);
                    free(version);
                    break;
                }
                if (0 > stat(tmp, &buf)) {
                    /* cannot stat file */
                    OPAL_OUTPUT_VERBOSE((1, orcm_cfgi_base.output,
                                         "%s could not stat %s",
                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), tmp));
                    free(tmp);
                    free(binary);
                    free(version);
                    break;
                }
                vers->mod_time = strdup(ctime(&buf.st_mtime));
                /* strip the ending cr */
                vers->mod_time[strlen(vers->mod_time)-1] = '\0';
                OPAL_OUTPUT_VERBOSE((1, orcm_cfgi_base.output,
                                     "%s BIN %s MODTIME %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     binary, vers->mod_time));
            }
            free(version);
            break;

        case ORCM_CFGI_FILE_ARGV:
            /* if we don't have an active job, that's an error */
            if (NULL == app) {
                /* cannot process this */
                rc = ORTE_ERR_BAD_PARAM;
                goto depart;
            }
            /* if we don't have an active exec, that's an error */
            if (NULL == exec) {
                /* cannot process this */
                OBJ_RELEASE(app);
                rc = ORTE_ERR_BAD_PARAM;
                goto depart;
            }
            /* if we don't have an active version, that's an error */
            if (NULL == vers) {
                /* cannot process this */
                OBJ_RELEASE(app);
                rc = ORTE_ERR_BAD_PARAM;
                goto depart;
            }
            if (NULL == (argv = parse_string())) {
                orte_show_help("help-cfgi-file.txt", "no-argv", true,
                               filename, orcm_cfgi_file_line, token,
                               orcm_cfgi_file_value.sval);
                OBJ_RELEASE(app);
                rc = ORTE_ERR_BAD_PARAM;
                goto depart;
            }
            vers->argv = opal_argv_split(argv, ' ');
            free(argv);
            break;

        case ORCM_CFGI_FILE_PROCESS_LIMIT:
            /* if we don't have an active job, that's an error */
            if (NULL == app) {
                /* cannot process this */
                rc = ORTE_ERR_BAD_PARAM;
                goto depart;
            }
            /* if we don't have an active exec, that's an error */
            if (NULL == exec) {
                /* cannot process this */
                OBJ_RELEASE(app);
                rc = ORTE_ERR_BAD_PARAM;
                goto depart;
            }
            /* set the process limit */
            if ((exec->process_limit = parse_int()) < 0) {
                orte_show_help("help-cfgi-file.txt", "bad-max-procs", true,
                               filename, orcm_cfgi_file_line, token, 
                               orcm_cfgi_file_value.sval);
                OBJ_RELEASE(app);
                rc = ORTE_ERR_BAD_PARAM;
                goto depart;
            }
            break;

        case ORCM_CFGI_FILE_STRING:
        case ORCM_CFGI_FILE_QUOTED_STRING:
            orte_show_help("help-cfgi-file.txt", "parse_error_string",
                           true,
                           filename, 
                           orcm_cfgi_file_line, 
                           token, 
                           orcm_cfgi_file_value.sval);
            break;

        case ORCM_CFGI_FILE_INT:
            orte_show_help("help-cfgi-file.txt", "parse_error_int",
                           true,
                           filename, 
                           orcm_cfgi_file_line, 
                           token, 
                           orcm_cfgi_file_value.ival);
            break;

        default:
            orte_show_help("help-cfgi-file.txt", "parse_error",
                           true,
                           filename, 
                           orcm_cfgi_file_line, 
                           token);
        }
    }

 depart:
    fclose(orcm_cfgi_file_in);
    orcm_cfgi_file_done = false;
    return rc;
}

#ifdef HAVE_SYS_INOTIFY_H
static void inotify_handler(int fd, short args, void *cbdata)
{
    struct {
        struct inotify_event ev;
        char filename_buffer[4096];
    } data;
    int sz;
    struct timespec tp={0, 100000};

    sz= read(fd, &data, sizeof(data));

    if (sz >= sizeof(data.ev)) {

        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s Inotify Event: name %s type %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             data.ev.name, data.ev.mask));
        /* if the event involves a typical editor swap
         * file, ignore it
         */
        if (NULL != strstr(data.ev.name, ".swp") ||
            NULL != strchr(data.ev.name, '~')) {
            /* vi swap */
            OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                 "%s Inotify Event: Ignoring .swp file",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
            return;
        }
        if ('.' == data.ev.name[0]) {
            /* special file */
            OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                 "%s Inotify Event: Ignoring dot-file",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
            return;
        }
        if ('#' == data.ev.name[0]) {
            /* emacs auto-save */
            OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                 "%s Ignoring emacs auto-save file",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
            return;
        }
        /* ensure that editors have a chance to cleanly
         * exit before reading the config so we don't
         * destabilize the parser
         */
        nanosleep(&tp, NULL);
        check_config(0, 0, NULL);
    } else if (sz < 0) {
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s Inotify Event: READ ERROR",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME))); /* EINVAL: short of space */
        return;
    } else {
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s Inotify Event: Short read: %d", 
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), sz));
    }
}
#endif

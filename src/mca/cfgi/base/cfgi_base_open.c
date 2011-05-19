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

#include "opal/util/output.h"
#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"

#include "runtime/runtime.h"

#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/base/public.h"
#include "mca/cfgi/base/private.h"
#include "mca/cfgi/base/components.h"

/* stubs */
static int orcm_stub_init(void);
static int orcm_stub_finalize(void);

/* instantiate the module */
orcm_cfgi_base_module_t orcm_cfgi = {
    orcm_stub_init,
    orcm_stub_finalize,
    orcm_cfgi_base_activate
};

/* instantiate the globals */
orcm_cfgi_base_t orcm_cfgi_base;
opal_list_t orcm_cfgi_components_available;
opal_list_t orcm_cfgi_selected_modules;

int orcm_cfgi_base_open(void)
{
    /* Debugging / verbose output.  Always have stream open, with
     verbose set by the mca open system... */
    orcm_cfgi_base.output = opal_output_open(NULL);
    
    /* init the globals */
    OBJ_CONSTRUCT(&orcm_cfgi_base.ctl, orte_thread_ctl_t);
    orcm_cfgi_base.num_active_apps = 0;
    orcm_cfgi_base.daemons = NULL;
    OBJ_CONSTRUCT(&orcm_cfgi_components_available, opal_list_t);
    OBJ_CONSTRUCT(&orcm_cfgi_selected_modules, opal_list_t);
    OBJ_CONSTRUCT(&orcm_cfgi_base.installed_apps, opal_pointer_array_t);
    opal_pointer_array_init(&orcm_cfgi_base.installed_apps, 16, INT_MAX, 16);
    OBJ_CONSTRUCT(&orcm_cfgi_base.confgd_apps, opal_pointer_array_t);
    opal_pointer_array_init(&orcm_cfgi_base.confgd_apps, 16, INT_MAX, 16);
    orcm_cfgi_base.launch_pipe[0] = -1;
    orcm_cfgi_base.launch_pipe[1] = -1;

    /* Open up all available components */
    if (ORCM_SUCCESS != 
        mca_base_components_open("orcm_cfgi", orcm_cfgi_base.output, NULL,
                                 &orcm_cfgi_components_available, true)) {
            return ORCM_ERROR;
        }
    
    /* All done */
    return ORCM_SUCCESS;
}

static int orcm_stub_init(void)
{
    return ORCM_SUCCESS;
}

static int orcm_stub_finalize(void)
{
    return ORCM_SUCCESS;
}

OBJ_CLASS_INSTANCE(orcm_cfgi_base_selected_module_t,
                   opal_list_item_t,
                   NULL, NULL);

static void caddy_const(orcm_cfgi_caddy_t *ptr)
{
    ptr->cleanup = false;
    ptr->run = NULL;
    ptr->jdata = NULL;
}
static void caddy_dest(orcm_cfgi_caddy_t *ptr)
{
    if (NULL != ptr->run) {
        OBJ_RELEASE(ptr->run);
    }
    if (NULL != ptr->jdata) {
        OBJ_RELEASE(ptr->jdata);
    }
}
OBJ_CLASS_INSTANCE(orcm_cfgi_caddy_t,
                   opal_object_t,
                   caddy_const, caddy_dest);

static void vers_constructor(orcm_cfgi_version_t *ptr)
{
    ptr->idx = -1;
    ptr->exec = NULL;
    ptr->version = NULL;
    ptr->argv = NULL;
    OBJ_CONSTRUCT(&ptr->binaries, opal_pointer_array_t);
    opal_pointer_array_init(&ptr->binaries, 4, INT_MAX, 4);
}
static void vers_destructor(orcm_cfgi_version_t *ptr)
{
    int i;
    orcm_cfgi_bin_t *bin;

    /* NEVER release the exec field as this
     * will create a fatal recursion
     */
    if (NULL != ptr->exec && 0 <= ptr->idx) {
        opal_pointer_array_set_item(&ptr->exec->versions, ptr->idx, NULL);
    }
    if (NULL != ptr->version) {
        free(ptr->version);
    }
    if (NULL != ptr->argv) {
        opal_argv_free(ptr->argv);
    }

    /* don't need to release the contents of the binaries array as they
     * took care of that themselves
     */
    OBJ_DESTRUCT(&ptr->binaries);
}
OBJ_CLASS_INSTANCE(orcm_cfgi_version_t,
                   opal_object_t,
                   vers_constructor,
                   vers_destructor);

static void exec_constructor(orcm_cfgi_exec_t *ptr)
{
    ptr->idx = -1;
    ptr->appname = NULL;
    ptr->process_limit = -1;
    ptr->total_procs = 0;
    OBJ_CONSTRUCT(&ptr->versions, opal_pointer_array_t);
    opal_pointer_array_init(&ptr->versions, 4, INT_MAX, 4);
}
static void exec_destructor(orcm_cfgi_exec_t *ptr)
{
    int i;
    orcm_cfgi_version_t *vers;

    if (NULL != ptr->appname) {
        free(ptr->appname);
    }
    for (i=0; i < ptr->versions.size; i++) {
        if (NULL == (vers = (orcm_cfgi_version_t*)opal_pointer_array_get_item(&ptr->versions, i))) {
            continue;
        }
        OBJ_RELEASE(vers);
    }
    OBJ_DESTRUCT(&ptr->versions);
}
OBJ_CLASS_INSTANCE(orcm_cfgi_exec_t,
                   opal_object_t,
                   exec_constructor,
                   exec_destructor);

static void app_constructor(orcm_cfgi_app_t *ptr)
{
    ptr->modified = false;
    ptr->idx = -1;
    ptr->application = NULL;
    ptr->max_instances = -1;
    ptr->num_instances = 0;
    OBJ_CONSTRUCT(&ptr->executables, opal_pointer_array_t);
    opal_pointer_array_init(&ptr->executables, 4, INT_MAX, 4);
    OBJ_CONSTRUCT(&ptr->instances, opal_pointer_array_t);
    opal_pointer_array_init(&ptr->instances, 4, INT_MAX, 4);
}
static void app_destructor(orcm_cfgi_app_t *ptr)
{
    int i;
    orcm_cfgi_exec_t *exec;
    orcm_cfgi_run_t *run;

    if (NULL != ptr->application) {
        free(ptr->application);
    }
    for (i=0; i < ptr->executables.size; i++) {
        if (NULL == (exec = (orcm_cfgi_exec_t*)opal_pointer_array_get_item(&ptr->executables, i))) {
            continue;
        }
        OBJ_RELEASE(exec);
    }
    OBJ_DESTRUCT(&ptr->executables);
    for (i=0; i < ptr->instances.size; i++) {
        if (NULL == (run = (orcm_cfgi_run_t*)opal_pointer_array_get_item(&ptr->instances, i))) {
            continue;
        }
        OBJ_RELEASE(run);
    }
    OBJ_DESTRUCT(&ptr->instances);
}
OBJ_CLASS_INSTANCE(orcm_cfgi_app_t,
                   opal_object_t,
                   app_constructor,
                   app_destructor);

static void bin_constructor(orcm_cfgi_bin_t *ptr)
{
    ptr->idx = -1;
    ptr->vers_idx = -1;
    ptr->vers = NULL;
    ptr->exec = NULL;
    ptr->version = NULL;
    ptr->appname = NULL;
    ptr->binary = NULL;
    ptr->num_procs = 0;
}
static void bin_destructor(orcm_cfgi_bin_t *ptr)
{
    /* NEVER release the vers or exec fields as this
     * will create a fatal recursion
     */

    /* update number of procs */
    if (NULL != ptr->exec) {
        ptr->exec->total_procs -= ptr->num_procs;
    }

    if (NULL != ptr->vers && 0 <= ptr->vers_idx) {
        opal_pointer_array_set_item(&ptr->vers->binaries, ptr->vers_idx, NULL);
    }

    if (NULL != ptr->version) {
        free(ptr->version);
    }
    if (NULL != ptr->appname) {
        free(ptr->appname);
    }
    if (NULL != ptr->binary) {
        free(ptr->binary);
    }
}
OBJ_CLASS_INSTANCE(orcm_cfgi_bin_t,
                   opal_object_t,
                   bin_constructor,
                   bin_destructor);

static void run_constructor(orcm_cfgi_run_t *ptr)
{
    ptr->idx = -1;
    ptr->app_idx = -1;
    ptr->app = NULL;
    ptr->application = NULL;
    ptr->instance = NULL;
    OBJ_CONSTRUCT(&ptr->binaries, opal_pointer_array_t);
    opal_pointer_array_init(&ptr->binaries, 4, INT_MAX, 4);
}
static void run_destructor(orcm_cfgi_run_t *ptr)
{
    int i;
    orcm_cfgi_bin_t *bin;

    /* NEVER release the app field as this
     * will create a fatal recursion
     */

    /* cleanup the configured apps array */
    if (0 <= ptr->idx) {
        opal_pointer_array_set_item(&orcm_cfgi_base.confgd_apps, ptr->idx, NULL);
    }

    /* remove the instance */
    if (NULL != ptr->app) {
        /* decrement the number of instances */
        ptr->app->num_instances--;
        /* clear the array entry */
        if (0 <= ptr->app_idx) {
            opal_pointer_array_set_item(&ptr->app->instances, ptr->app_idx, NULL);
        }
    }

    for (i=0; i < ptr->binaries.size; i++) {
        if (NULL == (bin = (orcm_cfgi_bin_t*)opal_pointer_array_get_item(&ptr->binaries, i))) {
            continue;
        }
        OBJ_RELEASE(bin);
    }
    OBJ_DESTRUCT(&ptr->binaries);

    if (NULL != ptr->application) {
        free(ptr->application);
    }

    if (NULL != ptr->instance) {
        free(ptr->instance);
    }

}
OBJ_CLASS_INSTANCE(orcm_cfgi_run_t,
                   opal_object_t,
                   run_constructor,
                   run_destructor);

void orcm_cfgi_run_cleanup(orcm_cfgi_run_t *run)
{
}

void orcm_cfgi_base_dump(char **dumped, char *pfx, void *ptr, int type)
{
    orcm_cfgi_app_t *app;
    orcm_cfgi_exec_t *exec;
    orcm_cfgi_version_t *vers;
    orcm_cfgi_run_t *run;
    orcm_cfgi_bin_t *bin;
    orcm_cfgi_caddy_t *caddy;
    orte_job_t *jdt;
    int i;
    char *output, *tmp, *tmp2, *pfx2;

    /* protection */
    if (NULL == ptr) {
        opal_output(0, "DUMP WITH NULL PTR");
        return;
    }

    switch(type) {
    case ORCM_CFGI_APP:
        app = (orcm_cfgi_app_t*)ptr;
        /* create the string output */
        asprintf(&output, "%sApplication: %s\tMax instances: %d\tNum instances: %d",
                 (NULL == pfx) ? "" : pfx,
                 (NULL == app->application) ? "NULL" : app->application,
                 app->max_instances, app->num_instances);
        asprintf(&pfx2, "%s    ", (NULL == pfx) ? "" : pfx);
        for (i=0; i < app->executables.size; i++) {
            if (NULL == (exec = (orcm_cfgi_exec_t*)opal_pointer_array_get_item(&app->executables, i))) {
                continue;
            }
            orcm_cfgi_base_dump(&tmp, pfx2, exec, ORCM_CFGI_EXEC);
            asprintf(&tmp2, "%s\n%s", output, tmp);
            free(tmp);
            free(output);
            output = tmp2;
        }
        for (i=0; i < app->instances.size; i++) {
            if (NULL == (run = (orcm_cfgi_run_t*)opal_pointer_array_get_item(&app->instances, i))) {
                continue;
            }
            orcm_cfgi_base_dump(&tmp, pfx2, run, ORCM_CFGI_RUN);
            asprintf(&tmp2, "%s\n%s", output, tmp);
            free(tmp);
            free(output);
            output = tmp2;
        }
        free(pfx2);
        break;

    case ORCM_CFGI_EXEC:
        exec = (orcm_cfgi_exec_t*)ptr;
        asprintf(&output, "%sExec: %s\tProc limit: %d\tTotal procs: %d",
                 (NULL == pfx) ? "" : pfx,
                 (NULL == exec->appname) ? "NULL" : exec->appname,
                 exec->process_limit, exec->total_procs);
        asprintf(&pfx2, "%s    ", (NULL == pfx) ? "" : pfx);
        for (i=0; i < exec->versions.size; i++) {
            if (NULL == (vers = (orcm_cfgi_version_t*)opal_pointer_array_get_item(&exec->versions, i))) {
                continue;
            }
            orcm_cfgi_base_dump(&tmp, pfx2, vers, ORCM_CFGI_VERSION);
            asprintf(&tmp2, "%s\n%s", output, tmp);
            free(tmp);
            free(output);
            output = tmp2;
        }
        free(pfx2);
        break;

    case ORCM_CFGI_VERSION:
        vers = (orcm_cfgi_version_t*)ptr;
        if (NULL != vers->argv) {
            tmp = opal_argv_join(vers->argv, ' ');
        } else {
            tmp = NULL;
        }
        asprintf(&output, "%sExec: %s\tVersion: %s\tArgv: %s",
                 (NULL == pfx) ? "" : pfx,
                 (NULL == vers->exec) ? "NULL" : ((NULL == vers->exec->appname) ? "NULL" : vers->exec->appname),
                 (NULL == vers->version) ? "NULL" : vers->version,
                 (NULL == tmp) ? "NULL" : tmp);
        if (NULL != tmp) {
            free(tmp);
        }
        asprintf(&pfx2, "%s    ", (NULL == pfx) ? "" : pfx);
        for (i=0; i < vers->binaries.size; i++) {
            if (NULL == (bin = (orcm_cfgi_bin_t*)opal_pointer_array_get_item(&vers->binaries, i))) {
                continue;
            }
            orcm_cfgi_base_dump(&tmp, pfx2, bin, ORCM_CFGI_BIN);
            asprintf(&tmp2, "%s\n%s", output, tmp);
            free(tmp);
            free(output);
            output = tmp2;
        }
        free(pfx2);
        break;

    case ORCM_CFGI_RUN:
        run = (orcm_cfgi_run_t*)ptr;
        /* create the string output */
        asprintf(&output, "%sApp: %s\tInstance: %s",
                 (NULL == pfx) ? "" : pfx,
                 (NULL == run->application) ? "NULL" : run->application,
                 (NULL == run->instance) ? "NULL" : run->instance);
        asprintf(&pfx2, "%s    ", (NULL == pfx) ? "" : pfx);
        for (i=0; i < run->binaries.size; i++) {
            if (NULL == (bin = (orcm_cfgi_bin_t*)opal_pointer_array_get_item(&run->binaries, i))) {
                continue;
            }
            orcm_cfgi_base_dump(&tmp, pfx2, bin, ORCM_CFGI_BIN);
            asprintf(&tmp2, "%s\n%s", output, tmp);
            free(tmp);
            free(output);
            output = tmp2;
        }
        free(pfx2);
        break;

    case ORCM_CFGI_BIN:
        bin = (orcm_cfgi_bin_t*)ptr;
        asprintf(&output, "%sApp: %s\tVersion: %s\tBinary: %s\tNum procs: %d",
                 (NULL == pfx) ? "" : pfx,
                 (NULL == bin->appname) ? "NULL" : bin->appname,
                 (NULL == bin->version) ? "NULL" : bin->version,
                 (NULL == bin->binary) ? "NULL" : bin->binary, bin->num_procs);
        break;

    case ORCM_CFGI_CADDY:
        caddy = (orcm_cfgi_caddy_t*)ptr;
        asprintf(&output, "%sCaddy Cmd: %d\tRun: %s\tJdata: %s",
                 (NULL == pfx) ? "" : pfx,
                 (int)caddy->cmd,
                 (NULL == caddy->run) ? "NULL" : "NON-NULL",
                 (NULL == caddy->jdata) ? "NULL" : ORTE_JOBID_PRINT(caddy->jdata->jobid));
        asprintf(&pfx2, "%s    ", (NULL == pfx) ? "" : pfx);
        if (NULL != caddy->run) {
            orcm_cfgi_base_dump(&tmp, pfx2, caddy->run, ORCM_CFGI_RUN);
            asprintf(&tmp2, "%s\n%s", output, tmp);
            free(tmp);
            free(output);
            output = tmp2;
        }
        break;

    default:
        opal_output(0, "%s UNRECOGNIZED TYPE %d",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), type);
    }

    if (NULL != dumped) {
        *dumped = output;
    } else {
        opal_output(0, "\n%s", output);
        free(output);
    }
}
    
bool orcm_cfgi_app_definition_valid(orcm_cfgi_app_t *app)
{
    int i, j;
    orcm_cfgi_exec_t *exec;
    orcm_cfgi_version_t *vers;

    if (NULL == app->application) {
        return false;
    }
    for (i=0; i < app->executables.size; i++) {
        if (NULL == (exec = (orcm_cfgi_exec_t*)opal_pointer_array_get_item(&app->executables, i))) {
            continue;
        }
        if (NULL == exec->appname) {
            return false;
        }
        for (j=0; j < exec->versions.size; j++) {
            if (NULL == (vers = (orcm_cfgi_version_t*)opal_pointer_array_get_item(&exec->versions, j))) {
                continue;
            }
            if (NULL == vers->version) {
                return false;
            }
        }
    }

    return true;
}

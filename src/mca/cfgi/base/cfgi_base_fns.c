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
#include "opal/class/opal_pointer_array.h"
#include "opal/util/output.h"
#include "opal/util/argv.h"
#include "opal/util/basename.h"
#include "opal/util/opal_environ.h"
#include "opal/mca/installdirs/installdirs.h"

#include "orte/runtime/orte_globals.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/plm/plm_types.h"
#include "orte/mca/plm/base/plm_private.h"
#include "orte/mca/ras/ras.h"
#include "orte/mca/rmaps/rmaps.h"
#include "orte/mca/odls/odls.h"
#include "orte/util/name_fns.h"
#include "orte/util/parse_options.h"
#include "orte/orted/orted.h"

#include "mca/pnp/pnp.h"

#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/base/public.h"
#include "mca/cfgi/base/private.h"

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

int orcm_cfgi_base_spawn_app(orte_job_t *jdata, bool overwrite)
{
    char *value;
    int32_t rc=ORCM_SUCCESS, n, j, k, m, p;
    opal_buffer_t *response, *bfr, *terms;
    orte_job_t *job, *jlaunch;
    orte_proc_t *proc, *proctmp;
    orte_node_t *node;
    orte_app_context_t *app, *app2;
    orte_vpid_t vpid;
    char *replicas;
    int32_t ljob;
    uint16_t jfam;
    char *cmd, *cptr;
    orte_daemon_cmd_flag_t command;
    orte_rml_tag_t rmltag=ORTE_RML_TAG_INVALID;
    int base_channel;
    bool newjob;
    orte_process_name_t name;
    bool apps_added=false;

    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                         "%s spawn:app: %s:%s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == jdata->name) ? "UNNAMED" : jdata->name,
                         (NULL == jdata->instance) ? " " : jdata->instance));
    
    /* see if this is a pre-existing job */
    jlaunch = NULL;
    newjob = false;
    if (NULL != jdata->instance) {
        for (j=0; j < orte_job_data->size; j++) {
            if (NULL == (job = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, j))) {
                continue;
            }
            if (NULL == job->name || NULL == job->instance) {
                continue;
            }
            if ((NULL == jdata->name || 0 == strcasecmp(job->name, jdata->name)) &&
                0 == strcasecmp(job->instance, jdata->instance)) {
                jlaunch = job;
                break;
            }
        }
    }
    if (NULL == jlaunch) {
        /* this is a new job */
        newjob = true;
        jlaunch = jdata;
        jdata->state = ORTE_JOB_STATE_INIT;
        /* assign a jobid to it - ensure that the algorithm
         * used here is consistent so that all orcmd's will return
         * the exact same value!
         */
        if (ORTE_SUCCESS != (rc = orte_plm_base_create_jobid(jdata))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* store it on the global job data pool */
        ljob = ORTE_LOCAL_JOBID(jdata->jobid);
        opal_pointer_array_set_item(orte_job_data, ljob, jdata);
        /* check and set recovery flags */
        if (!jdata->recovery_defined) {
            /* set to system defaults */
            jdata->enable_recovery = orte_enable_recovery;
        }
        /* set apps to defaults */
        for (j=0; j < jdata->apps->size; j++) {
            if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, j))) {
                continue;
            }
            if (!app->recovery_defined) {
                app->max_restarts = orte_max_restarts;
            }
        }

        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s spawn: new job %s created",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_JOBID_PRINT(jdata->jobid)));
    
    } else {
        /* this is an existing job - update its info. This can
         * consist of changing the #procs, adding/deleting
         * app_contexts, changing the restart limits, etc.
         */
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s spawn: existing job %s modified",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_JOBID_PRINT(jlaunch->jobid)));
        newjob = false;
        apps_added = false;
        for (j=0; j < jdata->apps->size; j++) {
            if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, j))) {
                continue;
            }
            /* find the matching app in the existing job */
            for (n=0; n < jlaunch->apps->size; n++) {
                if (NULL == (app2 = (orte_app_context_t*)opal_pointer_array_get_item(jlaunch->apps, n))) {
                    continue;
                }
                if (0 != strcmp(app2->app, app->app)) {
                    continue;
                }
                /* the num_procs in the provided job is the actual number we want,
                 * not an adjustment
                 */
                if (app->num_procs == app2->num_procs) {
                    /* nothing to change */
                    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                         "%s spawn: no change to num_procs for app %s",
                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                         opal_basename(app->app)));
                    continue;
                }
                if (app->num_procs > app2->num_procs) {
                    /* we are adding procs - just add them to the end of the proc array */
                    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                         "%s spawn: adding procs to app %s",
                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                         opal_basename(app->app)));
                    for (k=0; k < (app->num_procs - app2->num_procs); k++) {
                        proc = OBJ_NEW(orte_proc_t);
                        proc->name.jobid = jlaunch->jobid;
                        proc->name.vpid = jlaunch->num_procs;
                        proc->state = ORTE_PROC_STATE_RESTART;
                        proc->app_idx = app2->idx;
                        opal_pointer_array_set_item(jlaunch->procs, proc->name.vpid, proc);
                        jlaunch->num_procs++;
                    }
                    app2->num_procs = app->num_procs;
                    apps_added = true;
                } else {
                    /* we are subtracting procs - cycle thru the proc array
                     * and flag the reqd number for termination. We don't
                     * care which ones, so take the highest ranking ones
                     */
                    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                         "%s spawn: removing procs from app %s",
                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                         opal_basename(app->app)));
                    terms = OBJ_NEW(opal_buffer_t);
                    /* indicate the target DVM */
                    jfam = ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid);
                    opal_dss.pack(terms, &jfam, 1, OPAL_UINT16);
                    /* pack the command */
                    command = ORTE_DAEMON_KILL_LOCAL_PROCS;
                    if (ORTE_SUCCESS != (rc = opal_dss.pack(terms, &command, 1, ORTE_DAEMON_CMD))) {
                        ORTE_ERROR_LOG(rc);
                        return rc;
                    }
                    for (k=jlaunch->procs->size-1, m=0; 0 <= k && m < (app2->num_procs - app->num_procs); k--) {
                        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jlaunch->procs, k))) {
                            continue;
                        }
                        if (proc->app_idx == app2->idx) {
                            /* flag it for termination - we can't remove it from the
                             * various tracking structures just yet because (a) the
                             * process is still alive, and (b) we need the info
                             * in those structures to kill it. Fortunately, when
                             * we do finally kill it, the errmgr will take care
                             * of cleaning up the tracking structures
                             */
                            proc->state = ORTE_PROC_STATE_TERMINATE;
                            if (ORTE_SUCCESS != (rc = opal_dss.pack(terms, &proc->name, 1, ORTE_NAME))) {
                                ORTE_ERROR_LOG(rc);
                                return rc;
                            }
                            m++;
                        }
                    }
                    app2->num_procs = app->num_procs;
                    /* send it to the daemons */
                    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_SYS_CHANNEL,
                                                                 NULL, ORCM_PNP_TAG_COMMAND,
                                                                 NULL, 0, terms, cbfunc, NULL))) {
                        ORTE_ERROR_LOG(rc);
                    }
                }
            }
        }
        /* if any apps were added, then we need to remap and launch
         * the job so the daemons will know to start them
         */
        if (!apps_added && ORTE_JOB_STATE_RESTART != jdata->state) {
            /* nothing added and no restart requested, so we are done! */
            return rc;
        }
        /* flag that this is a "restart" so the map will be redone */
        jlaunch->state = ORTE_JOB_STATE_RESTART;
    }

    if (newjob) {
        /* assign each app_context a pair of input/output multicast
         * groups. We do this regardless of whether or not any procs
         * are local to us so that we keep the assignment consistent
         * across orcmds
         */
        base_channel = (2*orcm_cfgi_base.num_active_apps) + ORTE_RMCAST_DYNAMIC_CHANNELS;
        for (j=0; j < jlaunch->apps->size; j++) {
            if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jlaunch->apps, j))) {
                continue;
            }
            cptr = opal_basename(app->app);
            asprintf(&value, "%s:%d", cptr, ((2*orcm_cfgi_base.num_active_apps)+ORTE_RMCAST_DYNAMIC_CHANNELS));
            free(cptr);
            opal_setenv("OMPI_MCA_rmcast_base_group", value, true, &app->env);
            free(value);
            orcm_cfgi_base.num_active_apps++;
            /* tell the app to use the right ess module */
            opal_setenv("OMPI_MCA_ess", "orcmapp", true, &app->env);
        }
    }


    /* setup the map */
    if (NULL == jlaunch->map) {
        jlaunch->map = OBJ_NEW(orte_job_map_t);
    }
    /* note that we cannot launch daemons,
     * so the map can only contain existing nodes
     */
    jlaunch->map->policy |= ORTE_MAPPING_USE_VM;
    jlaunch->map->policy |= ORTE_MAPPING_BYNODE;

    /* map the applications */
    if (ORTE_SUCCESS != (rc = orte_rmaps.map_job(jlaunch))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }         

    if (0 < opal_output_get_verbosity(orcm_cfgi_base.output)) {
        opal_output(orcm_cfgi_base.output, "Launching app %s instance %s with jobid %s",
                    (NULL == jlaunch->name) ? "UNNAMED" : jlaunch->name,
                    (NULL == jlaunch->instance) ? " " : jlaunch->instance,
                    ORTE_JOBID_PRINT(jlaunch->jobid));

        for (j=0; j < jlaunch->apps->size; j++) {
            if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jlaunch->apps, j))) {
                continue;
            }
            if (newjob) {
                opal_output(orcm_cfgi_base.output, "    exec %s on mcast channels %d (recv) %d (xmit)",
                            app->app, base_channel, base_channel+1);
                base_channel += 2;
            } else {
                opal_output(orcm_cfgi_base.output, "    exec %s with num_procs %d",
                            app->app, app->num_procs);
            }
        }
        if (4 <  opal_output_get_verbosity(orcm_cfgi_base.output)) {
            orte_devel_level_output = true;
            opal_dss.dump(orcm_cfgi_base.output, jlaunch, ORTE_JOB);
        }
    }

    /* since we need to retain our own local map of what
     * processes are where, construct a launch msg for
     * this job - but we won't send it anywhere, we'll
     * just locally process it to launch our own procs,
     * if any
     */
    bfr = OBJ_NEW(opal_buffer_t);

    /* indicate the target DVM */
    jfam = ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid);
    opal_dss.pack(bfr, &jfam, 1, OPAL_UINT16);

    /* get the launch data */
    if (ORTE_SUCCESS != (rc = orte_odls.get_add_procs_data(bfr, jlaunch->jobid))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(bfr);
        goto cleanup;
    }

    /* send it to the daemons */
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_SYS_CHANNEL,
                                                 NULL, ORCM_PNP_TAG_COMMAND,
                                                 NULL, 0, bfr, cbfunc, NULL))) {
        ORTE_ERROR_LOG(rc);
    }

    OPAL_OUTPUT_VERBOSE((5, orcm_cfgi_base.output,
                         "%s spawn: job %s launched",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_JOBID_PRINT(jlaunch->jobid)));

 cleanup:
    return rc;
}

int orcm_cfgi_base_kill_app(orte_job_t *jdata)
{
    int32_t rc=ORCM_SUCCESS;
    opal_buffer_t *bfr;
    orte_daemon_cmd_flag_t command;
    orte_process_name_t name;
    uint16_t jfam;

    /* construct the cmd buffer */
    bfr = OBJ_NEW(opal_buffer_t);

    /* indicate the target DVM */
    jfam = ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid);
    opal_dss.pack(bfr, &jfam, 1, OPAL_UINT16);

    command = ORTE_DAEMON_KILL_LOCAL_PROCS;
    if (ORTE_SUCCESS != (rc = opal_dss.pack(bfr, &command, 1, ORTE_DAEMON_CMD))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }

    /* if the job is to be terminated, then we send a special name */
    if (ORTE_JOB_STATE_ABORT_ORDERED == jdata->state) {
        name.jobid = jdata->jobid;
        name.vpid = ORTE_VPID_WILDCARD;
        if (ORTE_SUCCESS != (rc = opal_dss.pack(bfr, &name, 1, ORTE_NAME))) {
            ORTE_ERROR_LOG(rc);
            return rc;
        }
    } else {
    }

    /* send it to the daemons */
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_SYS_CHANNEL,
                                                 NULL, ORCM_PNP_TAG_COMMAND,
                                                 NULL, 0, bfr, cbfunc, NULL))) {
        ORTE_ERROR_LOG(rc);
    }

    return rc;
}

void orcm_cfgi_base_activate(void)
{
    opal_list_item_t *item;
    orcm_cfgi_base_selected_module_t *module;
    orcm_cfgi_base_module_t *nmodule;

    for (item = opal_list_get_first(&orcm_cfgi_selected_modules);
         opal_list_get_end(&orcm_cfgi_selected_modules) != item;
         item = opal_list_get_next(item)) {
        module = (orcm_cfgi_base_selected_module_t*) item;
        nmodule = module->module;
        if (NULL != nmodule->activate) {
            nmodule->activate();
        }
    }
}

int orcm_cfgi_base_check_job(orte_job_t *jdat)
{
    int i;
    orte_app_context_t *app;
    char *str;

    /* must have at least one app */
    if (NULL == opal_pointer_array_get_item(jdat->apps, 0)) {
        return ORCM_ERR_NO_APP_SPECIFIED;
    }
    if (jdat->num_apps <= 0) {
        return ORCM_ERR_NO_APP_SPECIFIED;
    }
    /* we require that an executable and the number of procs be specified
     * for each app
     */
    for (i=0; i < jdat->apps->size; i++) {
        if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdat->apps, i))) {
            continue;
        }
        if (NULL == app->app || NULL == app->argv || 0 == opal_argv_count(app->argv)) {
            OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                 "%s MISSING APP INFO name %s argv %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 (NULL == app->app) ? "NULL" : app->app,
                                 (NULL == app->argv) ? "NULL" : app->argv[0]));
            return ORCM_ERR_NO_EXE_SPECIFIED;
        }
        if (app->num_procs <= 0) {
            OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                 "%s NUM PROCS NOT SET name %s np %d",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 (NULL == app->app) ? "NULL" : app->app,
                                 app->num_procs));
            return ORCM_ERR_INVALID_NUM_PROCS;
        }
        /* ensure we can find the executable */
        if (NULL == app->env || 0 == opal_argv_count(app->env)) {
            if (ORTE_SUCCESS != orte_util_check_context_app(app, environ)) {
                OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                     "%s EXEC NOT FOUND: NO ENV name %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     (NULL == app->app) ? "NULL" : app->app));
                return ORCM_ERR_EXE_NOT_FOUND;
            }
        } else {
            if (ORTE_SUCCESS != orte_util_check_context_app(app, app->env)) {
                str = opal_argv_join(app->env, ':');
                OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                     "%s EXEC NOT FOUND name %s env %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     (NULL == app->app) ? "NULL" : app->app, str));
                free(str);
                return ORCM_ERR_EXE_NOT_FOUND;
            }
        }
    }
    return ORCM_SUCCESS;
}

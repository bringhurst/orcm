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
#include "orte/util/error_strings.h"
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

static int spawn_app(orcm_cfgi_caddy_t *caddy, bool overwrite)
{
    char *value;
    int32_t rc=ORCM_SUCCESS, n, j, k, m, p;
    opal_buffer_t *response, *bfr, *terms;
    orte_job_t *job, *jlaunch, *jdata;
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
    bool apps_added=false, procs_added=false, app_removed, app_found, proc_found;

    /* convenience */
    jdata = caddy->jdata;

    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                         "%s spawn:app: %s:%s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == jdata->name) ? "UNNAMED" : jdata->name,
                         (NULL == jdata->instance) ? " " : jdata->instance));
    
    /* see if this is a pre-existing job */
    jlaunch = NULL;
    newjob = false;
    if (NULL != jdata->instance) {
        for (j=1; j < orte_job_data->size; j++) {
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
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s spawn:app: Launching new job %s:%s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (NULL == jdata->name) ? "UNNAMED" : jdata->name,
                             (NULL == jdata->instance) ? " " : jdata->instance));
        /* this is a new job */
        newjob = true;
        /* protect the job object so it doesn't get released
         * when the caddy does
         */
        caddy->jdata = NULL;
        jlaunch = jdata;
        jlaunch->state = ORTE_JOB_STATE_INIT;
        /* assign a jobid to it */
        if (ORTE_SUCCESS != (rc = orte_plm_base_create_jobid(jlaunch))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* store it on the global job data pool */
        ljob = ORTE_LOCAL_JOBID(jlaunch->jobid);
        opal_pointer_array_set_item(orte_job_data, ljob, jlaunch);
        /* check and set recovery flags */
        if (!jlaunch->recovery_defined) {
            /* set to system defaults */
            jlaunch->enable_recovery = orte_enable_recovery;
        }
        /* set apps to defaults */
        for (j=0; j < jlaunch->apps->size; j++) {
            if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jlaunch->apps, j))) {
                continue;
            }
            if (!app->recovery_defined) {
                app->max_restarts = orte_max_restarts;
            }
        }

        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s spawn: new job %s created",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_JOBID_PRINT(jlaunch->jobid)));
    
    } else {
        /* this is an existing job - update its info. This can
         * consist of changing the #procs, changing the restart limits, etc.
         */
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s spawn: existing job %s modified",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_JOBID_PRINT(jlaunch->jobid)));
        newjob = false;
        /* process any additions or updates */
        apps_added = false;
        procs_added = false;
        for (j=0; j < jdata->apps->size; j++) {
            if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, j))) {
                continue;
            }
            cmd = opal_basename(app->app);
            /* find the matching app in the existing job */
            app_found = false;
            for (n=0; n < jlaunch->apps->size; n++) {
                if (NULL == (app2 = (orte_app_context_t*)opal_pointer_array_get_item(jlaunch->apps, n))) {
                    continue;
                }
                cptr = opal_basename(app2->app);
                if (0 != strcmp(cptr, cmd)) {
                    free(cptr);
                    continue;
                }
                free(cptr);
                /* note that we found the app */
                app_found = true;
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
                    /* we are adding procs - look to fill-in empty vpids
                     * due to earlier terminations, and then add them to
                     * the end of the proc array if necessary
                     */
                    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                         "%s spawn: adding procs to app %s",
                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                         opal_basename(app->app)));
                    for (k=0; k < (app->num_procs - app2->num_procs); k++) {
                        /* look for the first "empty" slot */
                        proc_found = false;
                        for (p=0; p < jlaunch->procs->size; p++) {
                            if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jlaunch->procs, p))) {
                                continue;
                            }
                            if (ORTE_PROC_STATE_KILLED_BY_CMD == proc->state) {
                                /* this process was previously terminated by user
                                 * command, so we can reuse it here by simply
                                 * changing the state to RESTART. This helps
                                 * avoid continued growth of the procs array
                                 * in the use-case where we are just stopping
                                 * and restarting procs
                                 */
                                proc->state = ORTE_PROC_STATE_RESTART;
                                /* reset some values - rest will be reset
                                 * during the restart procedure
                                 */
                                proc->app_idx = app2->idx;
                                proc->restarts = 0;
                                jlaunch->num_procs++;
                                proc_found = true;
                            }
                        }
                        if (!proc_found) {
                            /* add it to the end of the proc array */
                            proc = OBJ_NEW(orte_proc_t);
                            proc->name.jobid = jlaunch->jobid;
                            proc->state = ORTE_PROC_STATE_RESTART;
                            proc->app_idx = app2->idx;
                            proc->name.vpid = opal_pointer_array_add(jlaunch->procs, proc);
                            jlaunch->num_procs++;
                        }
                    }
                    app2->num_procs = app->num_procs;
                    procs_added = true;
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
                            /* adjust number of procs in job */
                            jlaunch->num_procs--;
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
            if (!app_found) {
                /* this is a new executable - add it */
                OPAL_OUTPUT_VERBOSE((5, orcm_cfgi_base.output,
                                     "%s APP %s HAS BEEN ADDED TO JOB %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     app->app, jlaunch->name));
                opal_dss.copy((void**)&app2, app, ORTE_APP_CONTEXT);
                app2->idx = opal_pointer_array_add(jlaunch->apps, app2);
                /* because this is a running job, it will be remapped by the
                 * resilient mapper. This requires that we add proc objects for
                 * the app and set their state for "restart"
                 */
                for (k=0; k < app2->num_procs; k++) {
                    proc = OBJ_NEW(orte_proc_t);
                    proc->name.jobid = jlaunch->jobid;
                    proc->name.vpid = jlaunch->num_procs;
                    proc->state = ORTE_PROC_STATE_RESTART;
                    proc->app_idx = app2->idx;
                    opal_pointer_array_set_item(jlaunch->procs, proc->name.vpid, proc);
                    jlaunch->num_procs++;
                }
                if (!app2->recovery_defined) {
                    app2->max_restarts = orte_max_restarts;
                }
                app2->num_procs = app->num_procs;
                jlaunch->num_apps++;
                apps_added = true;
            }
            free(cmd);
        }
        /* if any apps were added, then we need to remap and launch
         * the job so the daemons will know to start them
         */
        if (!(apps_added || procs_added) && ORTE_JOB_STATE_RESTART != jdata->state) {
            /* nothing added and no restart requested, so we are done! */
            return rc;
        }
        /* flag that this is a "restart" so the map will be redone */
        jlaunch->state = ORTE_JOB_STATE_RESTART;
    }

    if (newjob || apps_added) {
        /* assign each app_context a pair of input/output multicast groups */
        base_channel = (2*orcm_cfgi_base.num_active_apps) + ORTE_RMCAST_DYNAMIC_CHANNELS;
        for (j=0; j < jlaunch->apps->size; j++) {
            if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jlaunch->apps, j))) {
                continue;
            }
            /* already assigned channels? */
            app_found = false;
            if (NULL != app->env) {
                for (k=0; NULL != app->env[k]; k++) {
                    if (0 == strncmp(app->env[k], "OMPI_MCA_rmcast_base_group", strlen("OMPI_MCA_rmcast_base_group"))) {
                        app_found = true;
                        break;
                    }
                }
            }
            if (!app_found) {
                cptr = opal_basename(app->app);
                asprintf(&value, "%s:%d", cptr, ((2*orcm_cfgi_base.num_active_apps)+ORTE_RMCAST_DYNAMIC_CHANNELS));
                free(cptr);
                opal_setenv("OMPI_MCA_rmcast_base_group", value, true, &app->env);
                free(value);
                orcm_cfgi_base.num_active_apps++;
            }
            /* tell the app to use the right ess module */
            opal_setenv("OMPI_MCA_ess", "orcmapp", true, &app->env);
            /* add the instance to the app's environment */
            opal_setenv("ORCM_APP_INSTANCE", jlaunch->instance, true, &app->env);
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

static void term_job(orte_job_t *jdat, opal_buffer_t *bfr)
{
    orte_process_name_t name;
    int rc;

    /* if the job is already terminated, then just remove it from
     * the array - if we try to tell it to die, nothing will
     * happen and thus the errmgr won't remove the job for us
     * Reserve the special case of procs_migrating as there
     * could be other procs still running
     */
    if (ORTE_JOB_STATE_UNTERMINATED < jdat->state &&
        ORTE_JOB_STATE_PROCS_MIGRATING != jdat->state) {
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s APP %s IS IN STATE %s - REMOVING FROM JOB ARRAY",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (NULL == jdat->name) ? "NULL" : jdat->name,
                             orte_job_state_to_str(jdat->state)));
        opal_pointer_array_set_item(orte_job_data, ORTE_LOCAL_JOBID(jdat->jobid), NULL);
        OBJ_RELEASE(jdat);
    } else {
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s ORDERING APP %s TO ABORT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (NULL == jdat->name) ? "NULL" : jdat->name));
        jdat->state = ORTE_JOB_STATE_ABORT_ORDERED;  /* flag that this job is to be aborted */
        /* since the job is to be terminated, we send a special name */
        name.jobid = jdat->jobid;
        name.vpid = ORTE_VPID_WILDCARD;
        if (ORTE_SUCCESS != (rc = opal_dss.pack(bfr, &name, 1, ORTE_NAME))) {
            ORTE_ERROR_LOG(rc);
        }
    }
}

static int kill_job(orcm_cfgi_caddy_t *caddy)
{
    int32_t rc=ORCM_SUCCESS;
    opal_buffer_t *bfr;
    orte_daemon_cmd_flag_t command;
    uint16_t jfam;
    orte_job_t *jdat;
    orcm_cfgi_run_t *run;
    int i;
    bool found=false;

    OPAL_OUTPUT_VERBOSE((5, orcm_cfgi_base.output,
                         "%s KILLING JOBS",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));

    /* construct the cmd buffer */
    bfr = OBJ_NEW(opal_buffer_t);

    /* indicate the target DVM */
    jfam = ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid);
    opal_dss.pack(bfr, &jfam, 1, OPAL_UINT16);

    command = ORTE_DAEMON_KILL_LOCAL_PROCS;
    if (ORTE_SUCCESS != (rc = opal_dss.pack(bfr, &command, 1, ORTE_DAEMON_CMD))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(bfr);
        return rc;
    }

    /* convenience */
    run = caddy->run;

    if (NULL == run) {
        /* terminate all running jobs */
        for (i=1; i < orte_job_data->size; i++) {
            if (NULL == (jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
                continue;
            }
            OPAL_OUTPUT_VERBOSE((5, orcm_cfgi_base.output,
                                 "%s KILLING JOB %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_JOBID_PRINT(jdat->jobid)));
            term_job(jdat, bfr);
            found = true;
        }
    } else if (NULL != caddy->jdata) {
        OPAL_OUTPUT_VERBOSE((5, orcm_cfgi_base.output,
                             "%s KILLING JOB %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_JOBID_PRINT(caddy->jdata->jobid)));
        term_job(caddy->jdata, bfr);
        found = true;
    } else {
        /* terminate a specific instance, which means a specific job */
        for (i=1; i < orte_job_data->size; i++) {
            if (NULL == (jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
                continue;
            }
            if (0 == strcmp(jdat->name, run->application) &&
                0 == strcmp(jdat->instance, run->instance)) {
                OPAL_OUTPUT_VERBOSE((5, orcm_cfgi_base.output,
                                     "%s KILLING JOB %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_JOBID_PRINT(jdat->jobid)));
                term_job(jdat, bfr);
                found = true;
                break;
            }
        }
    }

    /* if no matching running job was found, ignore this */
    if (!found) {
        OPAL_OUTPUT_VERBOSE((5, orcm_cfgi_base.output,
                             "%s NO JOBS TO KILL - IGNORING KILL_JOBS CMD",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        OBJ_RELEASE(bfr);
        return ORCM_SUCCESS;
    }

    /* send it to the daemons */
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_SYS_CHANNEL,
                                                 NULL, ORCM_PNP_TAG_COMMAND,
                                                 NULL, 0, bfr, cbfunc, NULL))) {
        ORTE_ERROR_LOG(rc);
    }

    return rc;
}

static int kill_exec(orcm_cfgi_caddy_t *caddy)
{
    int32_t rc=ORCM_SUCCESS;
    opal_buffer_t *bfr;
    orte_daemon_cmd_flag_t command;
    orte_process_name_t name;
    uint16_t jfam;
    orte_job_t *jdat;
    orte_proc_t *proc;
    orte_app_context_t *app;
    orcm_cfgi_run_t *run;
    orcm_cfgi_bin_t *bin;
    orcm_cfgi_exec_t *exec;
    int i, j, k, n;
    char *cptr;
    bool found=false;

    OPAL_OUTPUT_VERBOSE((5, orcm_cfgi_base.output,
                         "%s KILL EXEC",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    /* construct the cmd buffer */
    bfr = OBJ_NEW(opal_buffer_t);

    /* indicate the target DVM */
    jfam = ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid);
    opal_dss.pack(bfr, &jfam, 1, OPAL_UINT16);

    command = ORTE_DAEMON_KILL_LOCAL_PROCS;
    if (ORTE_SUCCESS != (rc = opal_dss.pack(bfr, &command, 1, ORTE_DAEMON_CMD))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(bfr);
        return rc;
    }

    /* convenience */
    run = caddy->run;

    /* find the referenced job */
    for (i=1; i < orte_job_data->size; i++) {
        if (NULL == (jdat = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
            continue;
        }
        if (0 == strcmp(jdat->name, run->application) &&
            0 == strcmp(jdat->instance, run->instance)) {
            /* cycle thru the provided binaries  */
            for (k=0; k < run->binaries.size; k++) {
                if (NULL == (bin = (orcm_cfgi_bin_t*)opal_pointer_array_get_item(&run->binaries, k))) {
                    continue;
                }
                if (NULL != bin->vers) {
                    /* this binary wasn't removed */
                    continue;
                }
                /* search the job for the matching app_context */
                for (j=0; j < jdat->apps->size; j++) {
                    if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdat->apps, j))) {
                        continue;
                    }
                    cptr = opal_basename(app->app);
                    if (0 == strcmp(cptr, bin->binary)) {
                        /* find all procs for this app */
                        for (n=0; n < jdat->procs->size; n++) {
                            if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdat->procs, n))) {
                                continue;
                            }
                            if (proc->app_idx == app->idx) {
                                OPAL_OUTPUT_VERBOSE((5, orcm_cfgi_base.output,
                                                     "%s KILLING PROC %s",
                                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                                     ORTE_NAME_PRINT(&proc->name)));
                                /* flag it for termination - we can't remove it from the
                                 * various tracking structures just yet because (a) the
                                 * process is still alive, and (b) we need the info
                                 * in those structures to kill it. Fortunately, when
                                 * we do finally kill it, the errmgr will take care
                                 * of cleaning up the tracking structures
                                 */
                                proc->state = ORTE_PROC_STATE_TERMINATE;
                                if (ORTE_SUCCESS != (rc = opal_dss.pack(bfr, &proc->name, 1, ORTE_NAME))) {
                                    ORTE_ERROR_LOG(rc);
                                    OBJ_RELEASE(bfr);
                                    return rc;
                                }
                                found = true;
                            }
                        }
                        /* update the number of procs */
                        app->num_procs = 0;
                        free(cptr);
                        break;
                    }
                    free(cptr);
                }
                /* if requested, remove the binary from the run config */
                if (caddy->cleanup) {
                    OPAL_OUTPUT_VERBOSE((5, orcm_cfgi_base.output,
                                         "%s REMOVING BIN %s:%s FROM RUN %s:%s AT INDEX %d",
                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                         bin->appname, bin->version, run->application, run->instance, k));
                    opal_pointer_array_set_item(&run->binaries, k, NULL);
                    OBJ_RELEASE(bin);
                }
            }
        }
    }

    /* if nothing was found, ignore this */
    if (!found) {
        OPAL_OUTPUT_VERBOSE((5, orcm_cfgi_base.output,
                             "%s NO PROCS TO KILL - IGNORING KILL_EXEC CMD",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        OBJ_RELEASE(bfr);
        return ORCM_SUCCESS;
    }

    /* send it to the daemons */
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_SYS_CHANNEL,
                                                 NULL, ORCM_PNP_TAG_COMMAND,
                                                 NULL, 0, bfr, cbfunc, NULL))) {
        ORTE_ERROR_LOG(rc);
    }

    return rc;
}

static void launcher(int fd, short sd, void *args)
{
    int rc;
    orcm_cfgi_caddy_t *caddy;

    if (ORCM_SUCCESS != (rc = opal_fd_read(orcm_cfgi_base.launch_pipe[0],
                                           sizeof(orcm_cfgi_caddy_t*), &caddy))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    if (ORCM_CFGI_SPAWN == caddy->cmd) {
        /* launch it */
        if (ORCM_SUCCESS != (rc = spawn_app(caddy, true))) {
            ORTE_ERROR_LOG(rc);
            opal_output(0, "SPECIFIED APP %s IS INVALID - IGNORING",
                        (NULL == caddy->jdata->name) ? "NULL" : caddy->jdata->name);
            /* remove the job from the global pool */
            opal_pointer_array_set_item(orte_job_data, ORTE_LOCAL_JOBID(caddy->jdata->jobid), NULL);
        }
    } else if (ORCM_CFGI_KILL_JOB == caddy->cmd) {
        if (ORCM_SUCCESS != (rc = kill_job(caddy))) {
            ORTE_ERROR_LOG(rc);
        }
    } else if (ORCM_CFGI_KILL_EXE == caddy->cmd) {
        if (ORCM_SUCCESS != (rc = kill_exec(caddy))) {
            ORTE_ERROR_LOG(rc);
        }
    } else {
        opal_output(0, "%s Unrecognized confd cmd",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    }
    OBJ_RELEASE(caddy);
}


void orcm_cfgi_base_activate(void)
{
    opal_list_item_t *item;
    orcm_cfgi_base_selected_module_t *module;
    orcm_cfgi_base_module_t *nmodule;

    if (orcm_cfgi_base.launch_pipe[0] < 0) {
        /* setup the launch event */
        if (pipe(orcm_cfgi_base.launch_pipe) < 0) {
            opal_output(0, "CANNOT OPEN LAUNCH PIPE");
            return;
        }
        opal_event_set(opal_event_base, &orcm_cfgi_base.launch_event,
                       orcm_cfgi_base.launch_pipe[0],
                       OPAL_EV_READ|OPAL_EV_PERSIST, launcher, NULL);
        opal_event_add(&orcm_cfgi_base.launch_event, 0);
    }

    /* activate the modules */
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
        if (app->num_procs < 0) {
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

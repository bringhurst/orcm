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

int orcm_cfgi_base_spawn_app(orte_job_t *jdata)
{
    char *value;
    int32_t rc=ORCM_SUCCESS, n, j;
    opal_buffer_t *response, bfr;
    orte_job_t *job, *jlaunch;
    orte_app_context_t *app;
    orte_proc_t *proc;
    orte_vpid_t vpid;
    char *replicas;
    int32_t ljob;
    uint16_t jfam;
    char *cmd;
    orte_proc_t *proctmp;
    orte_daemon_cmd_flag_t command;
    orte_rml_tag_t rmltag=ORTE_RML_TAG_INVALID;
    
    if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, 0))) {
        ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
        return ORTE_ERR_BAD_PARAM;
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                         "%s spawn:app: %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), app->app));
    
    /* see if this is a pre-existing job */
    jlaunch = NULL;
    if (NULL != jdata->name && NULL != jdata->instance) {
        for (j=0; j < orte_job_data->size; j++) {
            if (NULL == (job = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, j))) {
                continue;
            }
            if (NULL == job->name || NULL == job->instance) {
                continue;
            }
            if (0 == strcasecmp(job->name, jdata->name) &&
                0 == strcasecmp(job->instance, jdata->instance)) {
                jlaunch = job;
                break;
            }
        }
    }
    if (NULL == jlaunch) {
        /* this is a new job */
        jlaunch = jdata;
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
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_JOBID_PRINT(jdata->jobid)));
    }

    /* assign each app_context a pair of input/output multicast
     * groups. We do this regardless of whether or not any procs
     * are local to us so that we keep the assignment consistent
     * across orcmds
     */
    for (j=0; j < jlaunch->apps->size; j++) {
        if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jlaunch->apps, j))) {
            continue;
        }
        asprintf(&value, "%s:%d", app->app, ((2*orcm_cfgi_base.num_active_apps)+ORTE_RMCAST_DYNAMIC_CHANNELS));
        opal_setenv("OMPI_MCA_rmcast_base_group", value, true, &app->env);
        free(value);
        orcm_cfgi_base.num_active_apps++;
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

    /* since we need to retain our own local map of what
     * processes are where, construct a launch msg for
     * this job - but we won't send it anywhere, we'll
     * just locally process it to launch our own procs,
     * if any
     */
    OBJ_CONSTRUCT(&bfr, opal_buffer_t);
    /* insert the process cmd */
    command = ORTE_DAEMON_PROCESS_CMD;
    if (ORTE_SUCCESS != (rc = opal_dss.pack(&bfr, &command, 1, ORTE_DAEMON_CMD))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&bfr);
        goto cleanup;
    }
    /* add the jobid and an arbitrary tag to maintain ordering */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(&bfr, &jlaunch->jobid, 1, ORTE_JOBID))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&bfr);
        goto cleanup;
    }
    if (ORTE_SUCCESS != (rc = opal_dss.pack(&bfr, &rmltag, 1, ORTE_RML_TAG))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&bfr);
        goto cleanup;
    }
    /* insert the add_procs cmd */
    command = ORTE_DAEMON_ADD_LOCAL_PROCS;
    if (ORTE_SUCCESS != (rc = opal_dss.pack(&bfr, &command, 1, ORTE_DAEMON_CMD))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&bfr);
        goto cleanup;
    }
    /* get the launch data */
    if (ORTE_SUCCESS != (rc = orte_odls.get_add_procs_data(&bfr, jlaunch->jobid))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&bfr);
        goto cleanup;
    }
    /* deliver this to ourself.
     * We don't want to message ourselves as this can create circular logic
     * in the RML. Instead, this macro will set a zero-time event which will
     * cause the buffer to be processed by the cmd processor - probably will
     * fire right away, but that's okay
     * The macro makes a copy of the buffer, so it's okay to release it here
     */
    ORTE_MESSAGE_EVENT(ORTE_PROC_MY_NAME, &bfr, ORTE_RML_TAG_DAEMON, orte_daemon_cmd_processor);
    OBJ_DESTRUCT(&bfr);
    OPAL_OUTPUT_VERBOSE((5, orcm_cfgi_base.output,
                         "%s spawn: job %s launched",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_JOBID_PRINT(jlaunch->jobid)));

 cleanup:
    return rc;
}

int orcm_cfgi_base_kill_app(opal_buffer_t *buffer)
{
    int32_t rc=ORCM_SUCCESS, n, j;
    opal_buffer_t bfr;
    orte_job_t *jdata;
    orte_app_context_t *app;
    char *replicas;
    char *cmd;
    opal_pointer_array_t killapps;
    orte_proc_t *proctmp;
    orte_daemon_cmd_flag_t command;
    orte_rml_tag_t rmltag=ORTE_RML_TAG_INVALID;

    /* construct the cmd buffer */
    OBJ_CONSTRUCT(&bfr, opal_buffer_t);
    command = ORTE_DAEMON_KILL_LOCAL_PROCS;
    opal_dss.pack(&bfr, &command, 1, ORTE_DAEMON_CMD);
    /* construct the array of apps to kill */
    OBJ_CONSTRUCT(&killapps, opal_pointer_array_t);
    opal_pointer_array_init(&killapps, 8, INT_MAX, 8);
    n=1;
    j=0;
    while (ORTE_SUCCESS == (rc = opal_dss.unpack(buffer, &cmd, &n, OPAL_STRING))) {
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s kill cmd for app %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (NULL == cmd) ? "ALL" : cmd));
        if (NULL == cmd) {
            j=0;
            goto execute_kill;
        }

        /* unpack the replica info */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &replicas, &n, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup_kill;
        }
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s kill replicas %s for app %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (NULL == replicas) ? "ALL" : replicas, cmd));
        /* find all job data objects for this app - skip the daemon job
         * We have to check all the jobs because there could be multiple
         * invocations of the same application
         */
        for (n=1; n < orte_job_data->size; n++) {
            if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, n))) {
                continue;
            }
            if (jdata->state > ORTE_PROC_STATE_UNTERMINATED) {
                /* job is already terminated */
                continue;
            }
            /* retrieve the app */
            if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, 0))) {
                /* youch - this won't work */
                ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
                rc = ORTE_ERR_NOT_FOUND;
                goto cleanup_kill;
            }
            if (0 == strcasecmp(cmd, app->app)) {
                if (NULL == replicas) {
                    /* killall procs of this jobid */
                    proctmp = OBJ_NEW(orte_proc_t);
                    proctmp->name.jobid = jdata->jobid;
                    proctmp->name.vpid = ORTE_VPID_WILDCARD;
                    opal_pointer_array_add(&killapps, proctmp);
                    j++;
                } else {
                }

            }
        }
        if (NULL != replicas) {
            free(replicas);
        }
        n=1;
    }
    if (ORTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        ORTE_ERROR_LOG(rc);
    } else {
        rc = ORTE_SUCCESS;
    }
    /* now execute the kill cmd */
 execute_kill:
    opal_dss.pack(&bfr, &j, 1, OPAL_INT32);
    if (0 < j) {
        for (n=0; n < killapps.size; n++) {
            if (NULL != (proctmp = (orte_proc_t*)opal_pointer_array_get_item(&killapps, n))) {
                opal_dss.pack(&bfr, &(proctmp->name), 1, ORTE_NAME);
            }
        }
    }
    ORTE_MESSAGE_EVENT(ORTE_PROC_MY_NAME, &bfr, ORTE_RML_TAG_DAEMON, orte_daemon_cmd_processor);
 cleanup_kill:
    OBJ_DESTRUCT(&bfr);
    for (j=0; j < killapps.size; j++) {
        if (NULL != (proctmp = (orte_proc_t*)opal_pointer_array_get_item(&killapps, j))) {
            OBJ_RELEASE(proctmp);
        }
    }
    OBJ_DESTRUCT(&killapps);

    return rc;
}

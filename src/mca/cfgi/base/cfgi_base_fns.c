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

#include "mca/pnp/pnp.h"

#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/base/public.h"
#include "mca/cfgi/base/private.h"

void orcm_cfgi_base_spawn_app(char *cmd, bool add_procs, bool continuous, bool debug,
                              int restarts, int np, char *hosts, bool constrain)
{
    int rc, i, n;
    orte_job_t *jdata;
    orte_proc_t *proc;
    orte_app_context_t *app;
    char *param, *value;
    char cwd[OPAL_PATH_MAX];
    char **inpt;
    orte_daemon_cmd_flag_t command;
    opal_buffer_t buffer;
    int32_t ljob;
    uint16_t jfam;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                         "%s spawn:app: %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), cmd));
    
    if (NULL == orcm_cfgi_base.daemons) {
        orcm_cfgi_base.daemons = orte_get_job_data_object(0);
    }
    
    /* if we are adding procs, find the existing job object */
    if (add_procs) {
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s spawn: adding application",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        for (i=0; i < orte_job_data->size; i++) {
            if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
                continue;
            }
            if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, 0))) {
                continue;
            }
            if (0 == strcmp(cmd, app->app)) {
                /* found it */
                OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                     "%s spawn: found job %s - adding %d proc(s)",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_JOBID_PRINT(jdata->jobid), np));
                /* add the required number of proc objects to the jdata object */
                for (n=0; n < np; n++) {
                    proc = OBJ_NEW(orte_proc_t);
                    proc->name.jobid = jdata->jobid;
                    proc->name.vpid = jdata->num_procs++;
                    proc->app_idx = app->idx;
                    proc->state = ORTE_PROC_STATE_RESTART;
                    opal_pointer_array_set_item(jdata->procs, proc->name.vpid, proc);
                }
                /* increment num procs */
                app->num_procs += np;
                /* set the state to restart so we don't think it's a new job */
                jdata->state = ORTE_JOB_STATE_RESTART;
                goto launch;
            }
        }
    }
    /* get here if we are not adding procs, or we couldn't find the
     * pre-existing job object
     */
    
    /* create a new job for this app */
    jdata = OBJ_NEW(orte_job_t);
    if (ORTE_SUCCESS != (rc = orte_plm_base_create_jobid(jdata))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(jdata);
        return;
    }
    /* store it on the global job data pool */
    ljob = ORTE_LOCAL_JOBID(jdata->jobid);
    opal_pointer_array_set_item(orte_job_data, ljob, jdata);
    
    /* break the cmd line down */
    inpt = opal_argv_split(cmd, ' ');
    /* setup the required info */
    app = OBJ_NEW(orte_app_context_t);
    app->app = strdup(inpt[0]);
    opal_argv_append_nosize(&app->argv, inpt[0]);
    /* copy any args */
    for (i=1; NULL != inpt[i]; i++) {
        opal_argv_append_nosize(&app->argv, inpt[i]);
    }
    /* done with this */
    opal_argv_free(inpt);
    
    /* get the cwd */
    if (OPAL_SUCCESS != opal_getcwd(cwd, sizeof(cwd))) {
        opal_output(0, "failed to get cwd");
        return;
    }
    app->cwd = strdup(cwd);
    app->num_procs = np;
    app->prefix_dir = strdup(opal_install_dirs.prefix);
    /* setup the hosts */
    if (NULL != hosts) {
        app->dash_host = opal_argv_split(hosts, ' ');
    }
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
    /* assign this group of apps a multicast group */
    asprintf(&value, "%s:%d", app->app, (orcm_cfgi_base.num_active_apps+ORTE_RMCAST_DYNAMIC_CHANNELS));
    opal_setenv("OMPI_MCA_rmcast_base_group", value, true, &app->env);
    free(value);
    orcm_cfgi_base.num_active_apps++;
    
    opal_pointer_array_add(jdata->apps, app);
    jdata->num_apps = 1;
    
    /* run the allocator */
    if (ORTE_SUCCESS != (rc = orte_ras.allocate(jdata))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(jdata);
        return;
    }
    
    /* indicate that this is to be a continuously operating job - i.e.,
     * we restart processes even if they normally terminate
     */
    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                         "%s setting controls to %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (continuous) ? "CONTINUOUS" : "NON-CONTINUOUS"));
    if (continuous) {
        jdata->controls |= ORTE_JOB_CONTROL_CONTINUOUS_OP;
    }
    /* we don't forward stdin */
    jdata->stdin_target = ORTE_VPID_INVALID;
    
    /* pass max number of restarts */
    jdata->max_global_restarts = restarts;
    
launch:
    /* if we want to debug the apps, set the proper envar */
    if (debug) {
        opal_setenv("ORCM_MCA_spin", "1", true, &app->env);
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                         "%s LAUNCHING APP %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), app->app));
    
    /* map it */
    if (ORTE_SUCCESS != (rc = orte_rmaps.map_job(jdata))) {
        ORTE_ERROR_LOG(rc);
        return;
    }         
    
    /* if we don't want to launch, then we are done */
    if (orte_do_not_launch) {
        return;
    }
    
    /* setup the buffer */
    OBJ_CONSTRUCT(&buffer, opal_buffer_t);
    
    /* include the job family of the target dvm */
    jfam  = ORTE_JOB_FAMILY(orcm_cfgi_base.daemons->jobid);
    opal_dss.pack(&buffer, &jfam, 1, OPAL_UINT16);
    
    /* get the local launcher's required data */
    if (ORTE_SUCCESS != (rc = orte_odls.get_add_procs_data(&buffer, jdata->jobid))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&buffer);
        return;
    }
    
    /* send it to the vm */
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_buffer(ORCM_PNP_SYS_CHANNEL,
                                                     NULL, ORCM_PNP_TAG_COMMAND,
                                                     &buffer))) {
        ORTE_ERROR_LOG(rc);
    }
    OBJ_DESTRUCT(&buffer);
    return;
}

int orcm_cfgi_base_kill_app(orte_job_t *jdata, char *replicas)
{
    int rc;
    opal_buffer_t cmd;
    orte_daemon_cmd_flag_t command=ORTE_DAEMON_KILL_LOCAL_PROCS;
    int i, v;
    char **vpids=NULL;
    orte_proc_t *proc;
    int32_t num_replicas=1;
    uint16_t jfam;
    bool wildcard=true;
    orte_process_name_t name;
    
    if (NULL == orcm_cfgi_base.daemons) {
        orcm_cfgi_base.daemons = orte_get_job_data_object(0);
    }
    
    OBJ_CONSTRUCT(&cmd, opal_buffer_t);
    
    /* include the job family of the target dvm */
    jfam  = ORTE_JOB_FAMILY(orcm_cfgi_base.daemons->jobid);
    opal_dss.pack(&cmd, &jfam, 1, OPAL_UINT16);
    
    if (NULL != replicas) {
        orte_util_parse_range_options(replicas, &vpids);
        wildcard = false;
        num_replicas = opal_argv_count(vpids);
    }
    
    /* pack the command */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(&cmd, &command, 1, ORTE_DAEMON_CMD))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&cmd);
        opal_argv_free(vpids);
        return rc;
    }
    
    /* pack the number of procs */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(&cmd, &num_replicas, 1, OPAL_INT32))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&cmd);
        opal_argv_free(vpids);
        return rc;
    }
    
    if (wildcard) {
        name.jobid = jdata->jobid;
        name.vpid = ORTE_VPID_WILDCARD;
        if (ORTE_SUCCESS != (rc = opal_dss.pack(&cmd, &name, 1, ORTE_NAME))) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&cmd);
            return rc;
        }
    } else {
        /* pack the proc names */
        for (i=0; NULL != vpids[i]; i++) {
            v = strtol(vpids[i], NULL, 10);
            if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, v))) {
                ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
                continue;
            }
            if (ORTE_SUCCESS != (rc = opal_dss.pack(&cmd, &(proc->name), 1, ORTE_NAME))) {
                ORTE_ERROR_LOG(rc);
                OBJ_DESTRUCT(&cmd);
                opal_argv_free(vpids);
                return rc;
            }
        }
        opal_argv_free(vpids);
    }
    
    /* send it to the vm */
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_buffer(ORCM_PNP_SYS_CHANNEL,
                                                     NULL, ORCM_PNP_TAG_COMMAND,
                                                     &cmd))) {
        ORTE_ERROR_LOG(rc);
    }
    OBJ_DESTRUCT(&cmd);
    
    return rc;
}

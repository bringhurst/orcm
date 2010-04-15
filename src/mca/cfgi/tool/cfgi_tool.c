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
#include "opal/util/path.h"

#include "orte/mca/rmcast/rmcast.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"

#include "mca/clip/clip.h"
#include "mca/pnp/pnp.h"

#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/base/private.h"
#include "mca/cfgi/base/public.h"
#include "mca/cfgi/tool/cfgi_tool.h"

/* API functions */

static int tool_init(void);
static int tool_finalize(void);

/* The module struct */

orcm_cfgi_base_module_t orcm_cfgi_tool_module = {
    tool_init,
    tool_finalize
};

/* local functions */
static void tool_messages(int status,
                          orte_process_name_t *sender,
                          orcm_pnp_tag_t tag,
                          opal_buffer_t *buffer,
                          void *cbdata);

static int tool_init(void)
{
    int ret;
    
    /* register to catch launch requests */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_input_buffer("orcm-start", "0.1", "alpha",
                                                              ORCM_PNP_SYS_CHANNEL,
                                                              ORCM_PNP_TAG_TOOL,
                                                              tool_messages))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    
    /* register to catch stop requests */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_input_buffer("orcm-stop", "0.1", "alpha",
                                                              ORCM_PNP_SYS_CHANNEL,
                                                              ORCM_PNP_TAG_TOOL,
                                                              tool_messages))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    
    return ORCM_SUCCESS;
}


static int tool_finalize(void)
{
    orcm_pnp.deregister_input("orcm-start", "0.1", "alpha",
                              ORCM_PNP_SYS_CHANNEL,
                              ORCM_PNP_TAG_TOOL);
    orcm_pnp.deregister_input("orcm-stop", "0.1", "alpha",
                              ORCM_PNP_SYS_CHANNEL,
                              ORCM_PNP_TAG_TOOL);
    
    return ORCM_SUCCESS;
}

/****    LOCAL FUNCTIONS    ****/
static void tool_messages(int status,
                          orte_process_name_t *sender,
                          orcm_pnp_tag_t tag,
                          opal_buffer_t *buffer,
                          void *cbdata)
{
    char *cmd, *hosts;
    int32_t rc=ORCM_SUCCESS, n, j, num_apps, restarts;
    opal_buffer_t response;
    orte_job_t *jdata;
    orte_app_context_t *app;
    orte_proc_t *proc;
    orte_vpid_t vpid;
    orcm_tool_cmd_t flag;
    int8_t constrain, add_procs, debug, continuous;
    char *replicas;
    int32_t ljob;
    uint16_t jfam;
    
    /* if this isn't intended for me or for the DVM I am scheduling, ignore it */
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &jfam, &n, OPAL_UINT16))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    if (jfam != ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid)) {
        opal_output(0, "%s NOT FOR ME!", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        return;
    }
    
    /* unpack the cmd */
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &flag, &n, ORCM_TOOL_CMD_T))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* setup the response */
    OBJ_CONSTRUCT(&response, opal_buffer_t);
    /* pack the job family of the sender so they know it is meant for them */
    jfam  = ORTE_JOB_FAMILY(sender->jobid);
    opal_dss.pack(&response, &jfam, 1, OPAL_UINT16);
    /* return the cmd flag */
    opal_dss.pack(&response, &flag, 1, ORCM_TOOL_CMD_T);
    
    if (ORCM_TOOL_START_CMD == flag) {
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s spawn cmd from %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(sender)));
        /* unpack the add procs flag */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &add_procs, &n, OPAL_INT8))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the debug flag */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &debug, &n, OPAL_INT8))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the continuous flag */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &continuous, &n, OPAL_INT8))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the max number of restarts */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &restarts, &n, OPAL_INT32))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the #instances to start */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &num_apps, &n, OPAL_INT32))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the starting hosts - okay to unpack a NULL string */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &hosts, &n, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the constrain flag */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &constrain, &n, OPAL_INT8))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* unpack the cmd */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &cmd, &n, OPAL_STRING))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        /* spawn it */
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s spawning cmd %s np %d hosts %s constrain %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             cmd, num_apps,
                             (NULL == hosts) ? "NULL" : hosts,
                             (0 == constrain) ? "FALSE" : "TRUE"));
        orcm_cfgi_base_spawn_app(cmd, add_procs, continuous, debug, restarts, num_apps, hosts, constrain);
    } else if (ORCM_TOOL_STOP_CMD == flag) {
        n=1;
        while (ORTE_SUCCESS == (rc = opal_dss.unpack(buffer, &cmd, &n, OPAL_STRING))) {
            OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                                 "%s kill cmd from %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(sender)));
            /* unpack the replica info */
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &replicas, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                goto cleanup;
            }
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
                    return;
                }
                if (0 == strcasecmp(cmd, app->app)) {
                    if (ORTE_SUCCESS != (rc = orcm_cfgi_base_kill_app(jdata, replicas))) {
                        ORTE_ERROR_LOG(rc);
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
    } else {
        opal_output(0, "%s: UNKNOWN TOOL CMD FLAG %d", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)flag);
    }
    
cleanup:
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_buffer(ORCM_PNP_SYS_CHANNEL,
                                                     NULL, ORCM_PNP_TAG_TOOL,
                                                     &response))) {
        ORTE_ERROR_LOG(rc);
    }
    OBJ_DESTRUCT(&response);
}

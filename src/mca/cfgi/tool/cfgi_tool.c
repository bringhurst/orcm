/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * The TOOL component receives pnp-based commands from tools and then
 * executes them. The component should only be selected by orcmd daemons.
 *
 * In the case of a "spawn" command, the tool receives a message from
 * orcm-start that contains the application(s) to be started. The
 * required data is extracted, and the resulting job is "mapped" to
 * determine where the specified processes shall be executed. The module
 * then checks the map to determine if this node is hosting any of the
 * processes - if so, it starts those processes. In either case, a
 * response is returned to orcm-start to indicate that the job has
 * locally been started. This message contains the number of orcmd's
 * currently known to exist in the system so that orcm-start can tell
 * when the procedure is complete.
 *
 * $HEADER$
 */

#include "openrcm_config_private.h"
#include "include/constants.h"

#include "opal/dss/dss.h"
#include "opal/class/opal_pointer_array.h"
#include "opal/util/output.h"
#include "opal/util/argv.h"
#include "opal/util/path.h"
#include "opal/threads/threads.h"

#include "orte/mca/ras/ras.h"
#include "orte/mca/rmaps/rmaps.h"
#include "orte/mca/rmcast/rmcast.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"
#include "orte/runtime/orte_wait.h"

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
                          struct iovec *msg, int count,
                          opal_buffer_t *buffer,
                          void *cbdata);

static int tool_init(void)
{
    int ret;
    
    /* register to catch launch requests */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcm-start", "0.1", "alpha",
                                                         ORCM_PNP_GROUP_OUTPUT_CHANNEL,
                                                         ORCM_PNP_TAG_TOOL,
                                                         tool_messages))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    
    /* register to catch stop requests */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcm-stop", "0.1", "alpha",
                                                         ORCM_PNP_GROUP_OUTPUT_CHANNEL,
                                                         ORCM_PNP_TAG_TOOL,
                                                         tool_messages))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    
    /* register to catch launch_job requests */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcmrun", "0.1", "alpha",
                                                         ORCM_PNP_GROUP_OUTPUT_CHANNEL,
                                                         ORCM_PNP_TAG_TOOL,
                                                         tool_messages))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    
    OPAL_OUTPUT_VERBOSE((1, orcm_cfgi_base.output, "cfgi:tool initialized"));
    return ORCM_SUCCESS;
}


static int tool_finalize(void)
{
    /* cannot cancel the recvs as the pnp framework will
     * already have been closed
     */
    OPAL_OUTPUT_VERBOSE((1, orcm_cfgi_base.output, "cfgi:tool finalized"));
    return ORCM_SUCCESS;
}

/****    LOCAL FUNCTIONS    ****/
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

static void tool_messages(int status,
                          orte_process_name_t *sender,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg,
                          int count,
                          opal_buffer_t *buffer,
                          void *cbdata)
{
    int32_t rc=ORCM_SUCCESS, n;
    opal_buffer_t *response;
    orte_job_t *jdata;
    uint16_t jfam;
    orcm_tool_cmd_t flag=ORCM_TOOL_ILLEGAL_CMD;

    /* wait for any existing action to complete */
    OPAL_ACQUIRE_THREAD(&orcm_cfgi_base.lock, &orcm_cfgi_base.cond, &orcm_cfgi_base.active);
    OPAL_OUTPUT_VERBOSE((1, orcm_cfgi_base.output,
                         "%s cfgi:tool released to process cmd",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));

    /* setup the response - we send it regardless so the tool won't hang */
    response = OBJ_NEW(opal_buffer_t);

    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &jfam, &n, OPAL_UINT16))) {
        ORTE_ERROR_LOG(rc);
        opal_dss.pack(response, &flag, 1, ORCM_TOOL_CMD_T);
        goto cleanup;
    }

    /* unpack the cmd */
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &flag, &n, ORCM_TOOL_CMD_T))) {
        ORTE_ERROR_LOG(rc);
        opal_dss.pack(response, &flag, 1, ORCM_TOOL_CMD_T);
        goto cleanup;
    }
    
    /* return the cmd flag */
    opal_dss.pack(response, &flag, 1, ORCM_TOOL_CMD_T);
    
    /* if this isn't intended for my DVM, ignore it */
    if (jfam != ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid)) {
        opal_output(0, "%s NOT FOR ME!", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        rc = ORTE_ERROR;
        goto cleanup;
    }
    
    if (ORCM_TOOL_START_CMD == flag) {
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s spawn cmd from %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(sender)));

        /* unpack the job object */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &jdata, &n, ORTE_JOB))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }

        /* launch the job */
        if (ORCM_SUCCESS != (rc = orcm_cfgi_base_spawn_app(jdata))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(jdata);
        }

    } else if (ORCM_TOOL_STOP_CMD == flag) {
        /* order the termination */
        rc = orcm_cfgi_base_kill_app(buffer);

    } else {
        opal_output(0, "%s: UNKNOWN TOOL CMD FLAG %d", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)flag);
    }
    
 cleanup:
    /* return the result of the cmd */
    opal_dss.pack(response, &rc, 1, OPAL_INT);
    /* release the thread */
    OPAL_RELEASE_THREAD(&orcm_cfgi_base.lock, &orcm_cfgi_base.cond, &orcm_cfgi_base.active);
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_SYS_CHANNEL,
                                                 sender, ORCM_PNP_TAG_TOOL,
                                                 NULL, 0, response, cbfunc, NULL))) {
        ORTE_ERROR_LOG(rc);
    }
}

/*
 * Copyright (c) 2010      Cisco Systems, Inc. All rights reserved.
 *
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */
#include "openrcm.h"
#include "constants.h"

#include "orte_config.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "opal/util/output.h"
#include "opal/util/argv.h"
#include "opal/dss/dss.h"

#include "orte/threads/threads.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"
#include "orte/mca/errmgr/errmgr.h"

#include "mca/pnp/pnp.h"
#include "mca/leader/leader.h"
#include "util/triplets.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/errmgr/base/base.h"
#include "orte/mca/errmgr/base/errmgr_private.h"
#include "orte/mca/routed/routed.h"

#include "errmgr_orcmapp.h"


/*
 * Module functions: Global
 */
static int init(void);
static int finalize(void);

static int update_state(orte_jobid_t job,
                        orte_job_state_t jobstate,
                        orte_process_name_t *proc_name,
                        orte_proc_state_t state,
			pid_t pid,
                        orte_exit_code_t exit_code);

static int predicted_fault(opal_list_t *proc_list,
                           opal_list_t *node_list,
                           opal_list_t *suggested_nodes);

static int suggest_map_targets(orte_proc_t *proc,
                               orte_node_t *oldnode,
                               opal_list_t *node_list);

static int ft_event(int state);



/******************
 * ORCMAPP module
 ******************/
orte_errmgr_base_module_t orte_errmgr_orcmapp_module = {
    init,
    finalize,
    orte_errmgr_base_log,
    orte_errmgr_base_abort,
    update_state,
    predicted_fault,
    suggest_map_targets,
    ft_event
};

/*
 * Local functions and globals
 */
static orte_thread_ctl_t ctl;

static void notify_failure(int status,
                          orte_process_name_t *sender,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg,
                          int count,
                          opal_buffer_t *buf,
                          void *cbdata);

/************************
 * API Definitions
 ************************/
static int init(void)
{
    int rc=ORTE_SUCCESS;

    /* construct globals */
    OBJ_CONSTRUCT(&ctl, orte_thread_ctl_t);

    /* setup to recv proc failure notifications */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_receive("orcmd", "0.1", "alpha",
                                                        ORCM_PNP_ERROR_CHANNEL,
                                                        ORCM_PNP_TAG_ERRMGR,
                                                        notify_failure, NULL))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_receive("orcm-sched", "0.1", "alpha",
                                                        ORCM_PNP_ERROR_CHANNEL,
                                                        ORCM_PNP_TAG_ERRMGR,
                                                        notify_failure, NULL))) {
        ORTE_ERROR_LOG(rc);
    }

    return rc;
}

static int finalize(void)
{
    /* destruct globals */
    OBJ_DESTRUCT(&ctl);

    orcm_pnp.cancel_receive("orcmd", "0.1", "alpha",
                            ORCM_PNP_APP_PUBLIC_CHANNEL,
                            ORCM_PNP_TAG_ERRMGR);

    return ORTE_SUCCESS;
}

static int update_state(orte_jobid_t job,
                        orte_job_state_t jobstate,
                        orte_process_name_t *proc,
                        orte_proc_state_t state,
			pid_t pid,
                        orte_exit_code_t exit_code)
{
    /* look for comm failure - if it is our daemon, then we
     * cannot recover
     */
    if (ORTE_JOB_STATE_COMM_FAILED == jobstate) {
        /* see if this is our lifeline */
        if (ORTE_SUCCESS != orte_routed.route_lost(proc)) {
            return ORTE_ERR_UNRECOVERABLE;
        }
    }

    /* nothing to do */
    return ORTE_SUCCESS;
}

static int predicted_fault(opal_list_t *proc_list,
                           opal_list_t *node_list,
                           opal_list_t *suggested_nodes)
{
    return ORTE_ERR_NOT_IMPLEMENTED;
}

static int suggest_map_targets(orte_proc_t *proc,
                               orte_node_t *oldnode,
                               opal_list_t *node_list)
{
    return ORTE_ERR_NOT_IMPLEMENTED;
}

int ft_event(int state)
{
    return ORTE_SUCCESS;
}

/*****************
 * Local Functions
 *****************/
static void notify_failure(int status,
                           orte_process_name_t *sender,
                           orcm_pnp_tag_t tag,
                           struct iovec *msg,
                           int count,
                           opal_buffer_t *buf,
                           void *cbdata)
{
    orcm_triplet_t *trp;
    orte_process_name_t failed;
    int n, rc;

    if (ORCM_PROC_IS_TOOL) {
        /* just ignore it */
        return;
    }

    /* get the thread */
    ORTE_ACQUIRE_THREAD(&ctl);


    /* unpack the names of the failed procs */
    n=1;
    while (ORTE_SUCCESS == (rc = opal_dss.unpack(buf, &failed, &n, ORTE_NAME))) {

        /* get the triplet for this process */
        if (NULL == (trp = orcm_get_triplet_process(&failed))) {
            opal_output(0, "%s errmgr: FAILURE NOTIFICATION FOR %s, but no triplet found for that process",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&failed));
            ORTE_RELEASE_THREAD(&ctl);
            return;
        }

        OPAL_OUTPUT_VERBOSE((0, orte_errmgr_base.output,
                             "%s GOT FAILURE NOTIFICATION FOR %s (%s)",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&failed), trp->string_id));

        /* notify the leader framework in case comm leader needs to change - be
         * sure to release thread prior to call to avoid conflict
         */
        ORTE_RELEASE_THREAD(&trp->ctl);
        orcm_leader.proc_failed(trp->string_id, &failed);
    }
    if (ORCM_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        ORTE_ERROR_LOG(rc);
    }

    ORTE_RELEASE_THREAD(&ctl);
}


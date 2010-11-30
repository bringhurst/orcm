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

#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/threads/threads.h"

#include "mca/pnp/pnp.h"
#include "mca/leader/leader.h"
#include "util/triplets.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/errmgr/base/base.h"
#include "orte/mca/errmgr/base/errmgr_private.h"

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
static orte_thread_ctl_t local_thread;
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
    OBJ_CONSTRUCT(&local_thread, orte_thread_ctl_t);

    /* setup to recv proc failure notifications */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_receive("orcmd", "0.1", "alpha",
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
    OBJ_DESTRUCT(&local_thread);

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
    orcm_source_t *src;
    orte_process_name_t failed;
    char *stringid;
    int n, rc;

    /* get the thread */
    ORTE_ACQUIRE_THREAD(&local_thread);

    /* unpack the name of the failed proc */
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &failed, &n, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                         "%s GOT FAILURE NOTIFICATION FOR %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(&failed)));

    /* get the triplet */
    if (NULL == (trp = orcm_get_triplet_jobid(failed.jobid))) {
        opal_output(0, "%s errmgr: no triplet found for proc %s",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    ORTE_NAME_PRINT(&failed));
        ORTE_RELEASE_THREAD(&local_thread);
        return;
    }

    /* get the source corresponding to this process - create it
     * if missing
     */
    src = orcm_get_source(trp, &failed, true);
    /* flag the source as failed */
    src->alive = false;
    ORTE_RELEASE_THREAD(&src->ctl);

    /* save the stringid */
    stringid = strdup(trp->string_id);

    /* notify the leader framework in case comm leader needs to change - be
     * sure to release thread prior to call to avoid conflict
     */
    ORTE_RELEASE_THREAD(&trp->ctl);
    orcm_leader.proc_failed(stringid, &failed);

    free(stringid);
    ORTE_RELEASE_THREAD(&local_thread);
}


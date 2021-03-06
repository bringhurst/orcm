/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 *
 */

#include "openrcm_config_private.h"
#include "include/constants.h"

#include <sys/types.h>
#include <stdio.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "opal/util/argv.h"
#include "opal/util/if.h"
#include "opal/mca/paffinity/paffinity.h"
#include "opal/mca/sysinfo/sysinfo.h"
#include "opal/mca/sysinfo/base/base.h"
#include "opal/threads/mutex.h"
#include "opal/threads/condition.h"

#include "orte/mca/rmcast/base/base.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/odls/odls_types.h"
#include "orte/mca/plm/base/base.h"
#include "orte/mca/rml/base/base.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/routed/base/base.h"
#include "orte/mca/routed/routed.h"
#include "orte/util/show_help.h"
#include "orte/util/proc_info.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_wait.h"
#include "orte/runtime/orte_globals.h"

#include "orte/mca/ess/ess.h"
#include "orte/mca/ess/base/base.h"
#include "orte/mca/ess/orcmtool/ess_orcmtool.h"

#include "mca/pnp/base/public.h"
#include "mca/leader/base/public.h"

static int rte_init(void);
static int rte_finalize(void);
static void rte_abort(int status, bool report) __opal_attribute_noreturn__;
static uint8_t proc_get_locality(orte_process_name_t *proc);
static orte_vpid_t proc_get_daemon(orte_process_name_t *proc);
static char* proc_get_hostname(orte_process_name_t *proc);
static orte_local_rank_t proc_get_local_rank(orte_process_name_t *proc);
static orte_node_rank_t proc_get_node_rank(orte_process_name_t *proc);
static int update_pidmap(opal_byte_object_t *bo);
static int update_nidmap(opal_byte_object_t *bo);


orte_ess_base_module_t orte_ess_orcmtool_module = {
    rte_init,
    rte_finalize,
    rte_abort,
    proc_get_locality,
    proc_get_daemon,
    proc_get_hostname,
    proc_get_local_rank,
    proc_get_node_rank,
    update_pidmap,
    update_nidmap,
    NULL /* ft_event */
};

static int local_init(void);
static void local_fin(void);

static int rte_init(void)
{
    int ret;
    char *error = NULL;
    char *tmp=NULL;
    orte_jobid_t jobid=ORTE_JOBID_INVALID;
    orte_vpid_t vpid=ORTE_VPID_INVALID;
    int32_t jfam;

    /* run the prolog */
    if (ORTE_SUCCESS != (ret = orte_ess_base_std_prolog())) {
        error = "orte_ess_base_std_prolog";
        goto error;
    }
    
    /* if we were given a jobid, use it */
    mca_base_param_reg_string_name("orte", "ess_jobid", "Process jobid",
                                   true, false, NULL, &tmp);
    if (NULL != tmp) {
        if (ORTE_SUCCESS != (ret = orte_util_convert_string_to_jobid(&jobid, tmp))) {
            ORTE_ERROR_LOG(ret);
            error = "convert_jobid";
            goto error;
        }
        free(tmp);
        ORTE_PROC_MY_NAME->jobid = jobid;
    }
        
    /* if we were given a vpid, use it */
    mca_base_param_reg_string_name("orte", "ess_vpid", "Process vpid",
                                   true, false, NULL, &tmp);
    if (NULL != tmp) {
        if (ORTE_SUCCESS != (ret = orte_util_convert_string_to_vpid(&vpid, tmp))) {
            ORTE_ERROR_LOG(ret);
            error = "convert_vpid";
            goto error;
        }
        free(tmp);
        ORTE_PROC_MY_NAME->vpid = vpid;
    }
        
    /* if both were given, then we are done */
    if (ORTE_JOBID_INVALID == jobid ||
        ORTE_VPID_INVALID == vpid) {
        /* create our own name */
        if (ORTE_SUCCESS != (ret = orte_plm_base_open())) {
            ORTE_ERROR_LOG(ret);
            error = "orte_plm_base_open";
            goto error;
        }
        
        if (ORTE_SUCCESS != (ret = orte_plm_base_select())) {
            ORTE_ERROR_LOG(ret);
            error = "orte_plm_base_select";
            goto error;
        }
        if (ORTE_SUCCESS != (ret = orte_plm.set_hnp_name())) {
            ORTE_ERROR_LOG(ret);
            error = "orte_plm_set_hnp_name";
            goto error;
        }
        /* close the plm since we opened it to set our
         * name, but have no further use for it
         */
        orte_plm_base_close();
    }
        
    /* do the rest of the standard tool init */
    if (ORTE_SUCCESS != (ret = local_init())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_ess_tool_init";
        goto error;
    }
    
    return ORTE_SUCCESS;
    
 error:
    orte_show_help("help-orte-runtime.txt",
                   "orte_init:startup:internal-failure",
                   true, error, ORTE_ERROR_NAME(ret), ret);
    
    return ret;
}

static int rte_finalize(void)
{
    local_fin();
    
    return ORTE_SUCCESS;    
}

/*
 * If we are a orcmtool, it could be beneficial to get a core file, so
 * we call abort.
 */
static void rte_abort(int status, bool report)
{
    /* do NOT do a normal finalize as this will very likely
     * hang the process. We are aborting due to an abnormal condition
     * that precludes normal cleanup 
     *
     * We do need to do the following bits to make sure we leave a 
     * clean environment. Taken from orte_finalize():
     * - Assume errmgr cleans up child processes before we exit.
     */
    
    /* - Clean out the global structures 
     * (not really necessary, but good practice)
     */
    orte_proc_info_finalize();
    
    /* Now exit/abort */
    if (report) {
        abort();
    }
    
    /* otherwise, just exit */
    exit(status);
}

static uint8_t proc_get_locality(orte_process_name_t *proc)
{
    return OPAL_PROC_NON_LOCAL;
    
}

static orte_vpid_t proc_get_daemon(orte_process_name_t *proc)
{
    return ORTE_VPID_INVALID;
}

static char* proc_get_hostname(orte_process_name_t *proc)
{
    return NULL;
}

static orte_local_rank_t proc_get_local_rank(orte_process_name_t *proc)
{
    return ORTE_LOCAL_RANK_INVALID;
}

static orte_node_rank_t proc_get_node_rank(orte_process_name_t *proc)
{
    return ORTE_NODE_RANK_INVALID;
}

static int update_pidmap(opal_byte_object_t *bo)
{
    return ORTE_SUCCESS;
}

static int update_nidmap(opal_byte_object_t *bo)
{
    return ORTE_SUCCESS;
}

static int local_init(void)
{
    int ret;
    char *error;

    /* Setup the communication infrastructure */
    
    /* Runtime Messaging Layer */
    if (ORTE_SUCCESS != (ret = orte_rml_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_rml_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_rml_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_rml_base_select";
        goto error;
    }
    /* Routed system */
    if (ORTE_SUCCESS != (ret = orte_routed_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_routed_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_routed_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_routed_base_select";
        goto error;
    }
    
    /* multicast */
    if (ORTE_SUCCESS != (ret = orte_rmcast_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_rmcast_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_rmcast_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_rmcast_base_select";
        goto error;
    }
    
    /* setup the pnp framework */
    if (ORCM_SUCCESS != (ret = orcm_pnp_base_open())) {
        error = "pnp_open";
        goto error;
    }
    if (ORCM_SUCCESS != (ret = orcm_pnp_base_select())) {
        error = "pnp_select";
        goto error;
    }

    /* setup the leader framework */
    if (ORCM_SUCCESS != (ret = orcm_leader_base_open())) {
        error = "leader_open";
        goto error;
    }
    if (ORCM_SUCCESS != (ret = orcm_leader_base_select())) {
        error = "leader_select";
        goto error;
    }

    /* enable communication via the rml */
    if (ORTE_SUCCESS != (ret = orte_rml.enable_comm())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_rml.enable_comm";
        goto error;
    }
    
    /* setup the routed info - the selected routed component
     * will know what to do. 
     */
    if (ORTE_SUCCESS != (ret = orte_routed.init_routes(ORTE_PROC_MY_NAME->jobid, NULL))) {
        ORTE_ERROR_LOG(ret);
        error = "orte_routed.init_routes";
        goto error;
    }
    
    return ORTE_SUCCESS;

 error:
    orte_show_help("help-orte-runtime.txt",
                   "orte_init:startup:internal-failure",
                   true, error, ORTE_ERROR_NAME(ret), ret);
    
    return ret;
}

static void local_fin(void)
{
    orte_wait_finalize();

    /* close pnp */
    orcm_leader_base_close();
    orcm_pnp_base_close();

    orte_rmcast_base_close();
    orte_routed_base_close();
    orte_rml_base_close();

}

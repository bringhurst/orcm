/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2009      The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2010      The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 *
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
#include <stddef.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_QINFO_H
#include <qinfo.h>
#define QLIB_MAX_SLOTS_PER_RACK 16
#endif

#include "opal/hash_string.h"
#include "opal/util/argv.h"
#include "opal/util/if.h"
#include "opal/util/os_path.h"
#include "opal/mca/paffinity/paffinity.h"
#include "opal/mca/sysinfo/sysinfo.h"
#include "opal/mca/sysinfo/base/base.h"

#include "orte/threads/threads.h"
#include "orte/mca/rmcast/base/base.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/plm/base/base.h"
#include "orte/mca/plm/base/plm_private.h"
#include "orte/mca/rmaps/rmaps_types.h"
#include "orte/mca/rml/base/base.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/routed/routed.h"
#include "orte/mca/sensor/sensor.h"
#include "orte/util/show_help.h"
#include "orte/util/proc_info.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_wait.h"
#include "orte/runtime/orte_globals.h"
#include "orte/orted/orted.h"

#include "orte/mca/ess/ess.h"
#include "orte/mca/ess/base/base.h"
#include "orte/mca/ess/orcmd/ess_orcmd.h"

#include "runtime/runtime.h"

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


orte_ess_base_module_t orte_ess_orcmd_module = {
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

/*
 * Local variables & functions
 */
static orte_job_t *daemons;
static orte_thread_ctl_t ctl;
static uint32_t my_uid;


static void local_fin(void);
static int local_setup(void);
static void vm_tracker(orcm_info_t *vm);
static void vm_term(int status,
                    orte_process_name_t *sender,
                    orcm_pnp_tag_t tag,
                    struct iovec *msg, int count,
                    opal_buffer_t *buf,
                    void *cbdata);
static void vm_cmd(int status,
                   orte_process_name_t *sender,
                   orcm_pnp_tag_t tag,
                   struct iovec *msg,
                   int count,
                   opal_buffer_t *buffer,
                   void *cbdata);
static void recv_contact(int status,
                         orte_process_name_t *sender,
                         orcm_pnp_tag_t tag,
                         struct iovec *msg,
                         int count,
                         opal_buffer_t *buffer,
                         void *cbdata);

static int rte_init(void)
{
    int ret;
    char *error = NULL;
    char *tmp=NULL, *tailpiece;
    orte_jobid_t jobid=ORTE_JOBID_INVALID;
    orte_vpid_t vpid=ORTE_VPID_INVALID;
    int32_t jfam;

    OBJ_CONSTRUCT(&ctl, orte_thread_ctl_t);
    
    my_uid = (uint32_t)getuid();

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
    } else {
        /* if we were given a job family, use it */
        mca_base_param_reg_string_name("orte", "ess_job_family", "Job family",
                                       true, false, NULL, &tmp);
        if (NULL != tmp) {
            jfam = strtoul(tmp, &tailpiece, 10);
            if (UINT16_MAX < jfam || NULL != tailpiece) {
                /* use a string hash to restructure this to fit */
                OPAL_HASH_STR(tmp, jfam);
            }
            ORTE_PROC_MY_NAME->jobid = ORTE_CONSTRUCT_LOCAL_JOBID(jfam << 16, 0);
        }
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
        if (vpid < 2) {
            /* NOT ALLOWED - POTENTIAL CONFLICT WITH ORCM AND ORCM-SCHED */
            error = "disallowed_vpid";
            ret = ORTE_ERR_BAD_PARAM;
            goto error;
        }
    }
        
    /* if both were given, then we are done */
    if (ORTE_JOBID_INVALID != ORTE_PROC_MY_NAME->jobid &&
        ORTE_VPID_INVALID != ORTE_PROC_MY_NAME->vpid) {
        goto complete;
    }

#if HAVE_QINFO_H
    /* if we have qlib, then we can ask it for info by which we determine our
     * name based on provided rack location info
     */
    {
        qinfo_t *qinfo;

        if (NULL != (qinfo = get_qinfo())) {
            /* if we were given a jobid, then leave it alone */
            if (ORTE_JOBID_INVALID == ORTE_PROC_MY_NAME->jobid) {
                /* not given - assign it to 0 */
                ORTE_PROC_MY_NAME->jobid = 0;
            }
            /* must ensure that no daemon gets vpid 0 or 1 */
            ORTE_PROC_MY_NAME->vpid = (qinfo->rack * QLIB_MAX_SLOTS_PER_RACK) + qinfo->slot + 2;
            /* ensure that the HNP uri is NULL */
            if (NULL != orte_process_info.my_hnp_uri) {
                opal_output(0, "%s CONFLICTING NAME RESOLUTION - NO NAME GIVEN, BUT HNP SPECIFIED",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                error = "name conflict";
                ret = ORTE_ERR_FATAL;
                goto error;
            }
            OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                                 "GOT NAME %s FROM QINFO rack %d slot %d ",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 qinfo->rack, qinfo->slot));
            goto complete;
        }
    }
#endif

    /* we must have been given a vpid - we can get the jobid
     * in other ways
     */
    if (ORTE_VPID_INVALID == ORTE_PROC_MY_NAME->vpid) {
        /* we have an error */
        error = "missing vpid assignment";
        ret = ORTE_ERR_FATAL;
        goto error;
    }

    /* if we were given an HNP, we can get the jobid from
     * the HNP's name - this is decoded in proc_info.c during
     * the prolog
     */
    if (ORTE_JOBID_INVALID != ORTE_PROC_MY_HNP->jobid) {
        ORTE_PROC_MY_NAME->jobid = orte_process_info.my_hnp.jobid;
    } else {
        /* just fake it */
        ORTE_PROC_MY_NAME->jobid = 0;
    }

 complete:
    if (ORTE_SUCCESS != (ret = local_setup())) {
        ORTE_ERROR_LOG(ret);
        error = "local_setup";
        goto error;
    }

    OBJ_DESTRUCT(&ctl);
    
    return ORTE_SUCCESS;
    
 error:
    orte_show_help("help-orte-runtime.txt",
                   "orte_init:startup:internal-failure",
                   true, error, ORTE_ERROR_NAME(ret), ret);
    
    OBJ_DESTRUCT(&ctl);
    
    return ret;
}

static int rte_finalize(void)
{
    local_fin();
    return ORTE_SUCCESS;    
}

/*
 * If we are a orcm, it could be beneficial to get a core file, so
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
    orte_node_t *node;
    orte_proc_t *myproc;
    int i;
    
    /* get my node */
    node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, ORTE_PROC_MY_NAME->vpid);
    
    /* cycle through the array of local procs */
    for (i=0; i < node->procs->size; i++) {
        if (NULL == (myproc = (orte_proc_t*)opal_pointer_array_get_item(node->procs, i))) {
            continue;
        }
        if (myproc->name.jobid == proc->jobid &&
            myproc->name.vpid == proc->vpid) {
            OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                                 "%s ess:orcm: proc %s is LOCAL",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(proc)));
            return (OPAL_PROC_ON_NODE | OPAL_PROC_ON_CU | OPAL_PROC_ON_CLUSTER);
        }
    }
    
    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s ess:orcm: proc %s is REMOTE",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(proc)));
    
    return OPAL_PROC_NON_LOCAL;
    
}

static orte_proc_t* find_proc(orte_process_name_t *proc)
{
    orte_job_t *jdata;
    
    if (NULL == (jdata = orte_get_job_data_object(proc->jobid))) {
        opal_output(0, "CANNOT FIND JOB OBJECT FOR PROC %s", ORTE_NAME_PRINT(proc));
        return NULL;
    }

    return (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, proc->vpid);
}


static orte_vpid_t proc_get_daemon(orte_process_name_t *proc)
{
    orte_proc_t *pdata;
    
    if( ORTE_JOBID_IS_DAEMON(proc->jobid) ) {
        return proc->vpid;
    }

    /* get the job data */
     if (NULL == (pdata = find_proc(proc))) {
         return ORTE_VPID_INVALID;
     }
     
    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s ess:orcm: proc %s is hosted by daemon %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(proc),
                         ORTE_VPID_PRINT(pdata->node->daemon->name.vpid)));
    
    return pdata->node->daemon->name.vpid;
}

static char* proc_get_hostname(orte_process_name_t *proc)
{
    orte_proc_t *pdata;
    
    if (NULL == (pdata = find_proc(proc))) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        return NULL;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s ess:orcm: proc %s is on host %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(proc),
                         pdata->node->name));
    
    return pdata->node->name;
}

static orte_local_rank_t proc_get_local_rank(orte_process_name_t *proc)
{
    orte_proc_t *pdata;
    
    if (NULL == (pdata = find_proc(proc))) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        return ORTE_LOCAL_RANK_INVALID;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s ess:orcm: proc %s has local rank %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(proc),
                         (int)pdata->local_rank));
    
    return pdata->local_rank;
}

static orte_node_rank_t proc_get_node_rank(orte_process_name_t *proc)
{
    orte_proc_t *pdata;
    
    if (NULL == (pdata = find_proc(proc))) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        return ORTE_NODE_RANK_INVALID;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s ess:orcm: proc %s has node rank %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(proc),
                         (int)pdata->node_rank));
    
    return pdata->node_rank;
}

static int update_pidmap(opal_byte_object_t *bo)
{
    /* there is nothing to do here - the ORCM can resolve
     * all requests directly from its internal data. However,
     * we do need to free the data in the byte object to
     * be consistent with other modules
     */
    if (NULL != bo && NULL != bo->bytes) {
        free(bo->bytes);
    }
    return ORTE_SUCCESS;
}


static int update_nidmap(opal_byte_object_t *bo)
{
    /* there is nothing to do here - the ORCM can resolve
     * all requests directly from its internal data. However,
     * we do need to free the data in the byte object to
     * be consistent with other modules
     */
    if (NULL != bo && NULL != bo->bytes) {
        free(bo->bytes);
    }
    return ORTE_SUCCESS;
}


static int local_setup(void)
{
    int ret;
    char *error = NULL;
    int value;
    orte_proc_t *proc;
    orte_node_t *node;
    orte_app_context_t *app;

    /* initialize the global list of local children and job data */
    OBJ_CONSTRUCT(&orte_local_children, opal_list_t);
    OBJ_CONSTRUCT(&orte_local_jobdata, opal_list_t);
    
    /* determine the topology info */
    if (0 == orte_default_num_sockets_per_board) {
        /* we weren't given a number, so try to determine it */
        if (OPAL_SUCCESS != opal_paffinity_base_get_socket_info(&value)) {
            /* can't get any info - default to 1 */
            value = 1;
        }
        orte_default_num_sockets_per_board = (uint8_t)value;
    }
    if (0 == orte_default_num_cores_per_socket) {
        /* we weren't given a number, so try to determine it */
        if (OPAL_SUCCESS != opal_paffinity_base_get_core_info(0, &value)) {
            /* don't have topo info - can we at least get #processors? */
            if (OPAL_SUCCESS != opal_paffinity_base_get_processor_info(&value)) {
                /* can't get any info - default to 1 */
                value = 1;
            }
        }
        orte_default_num_cores_per_socket = (uint8_t)value;
    }
    
    /* setup the global job and node arrays */
    orte_job_data = OBJ_NEW(opal_pointer_array_t);
    if (ORTE_SUCCESS != (ret = opal_pointer_array_init(orte_job_data,
                                                       1,
                                                       ORTE_GLOBAL_ARRAY_MAX_SIZE,
                                                       1))) {
        ORTE_ERROR_LOG(ret);
        error = "setup job array";
        goto error;
    }
    
    orte_node_pool = OBJ_NEW(opal_pointer_array_t);
    if (ORTE_SUCCESS != (ret = opal_pointer_array_init(orte_node_pool,
                                                       ORTE_GLOBAL_ARRAY_BLOCK_SIZE,
                                                       ORTE_GLOBAL_ARRAY_MAX_SIZE,
                                                       ORTE_GLOBAL_ARRAY_BLOCK_SIZE))) {
        ORTE_ERROR_LOG(ret);
        error = "setup node array";
        goto error;
    }
    
    /* Setup the job data object for the daemons */        
    /* create and store the job data object */
    daemons = OBJ_NEW(orte_job_t);
    daemons->jobid = ORTE_PROC_MY_NAME->jobid;
    daemons->name = strdup("ORCM DVM");
    daemons->instance = strdup(ORTE_JOBID_PRINT(ORTE_PROC_MY_NAME->jobid));
    /* create an app */
    app = OBJ_NEW(orte_app_context_t);
    app->app = strdup("orcmd");
    opal_argv_append_nosize(&app->argv, "orcmd");
    /* add to the daemon job - always must be an app for a job */
    opal_pointer_array_add(daemons->apps, app);
    /* setup the daemon map so it knows how to map them */
    daemons->map = OBJ_NEW(orte_job_map_t);
    daemons->map->policy = ORTE_MAPPING_BYNODE;
    /* save it */
    opal_pointer_array_set_item(orte_job_data, 0, daemons);
   
    /* ensure our mapping policy will utilize any VM */
    ORTE_ADD_MAPPING_POLICY(ORTE_MAPPING_USE_VM);
    /* use bynode mapping by default */
    ORTE_ADD_MAPPING_POLICY(ORTE_MAPPING_BYNODE);

    /* create and store a node object where we are */
    node = OBJ_NEW(orte_node_t);
    node->name = strdup(orte_process_info.nodename);
    node->slots = 1;  /* min number */
    node->slots_alloc = node->slots;
    node->index = ORTE_PROC_MY_NAME->vpid;
    opal_pointer_array_set_item(orte_node_pool, ORTE_PROC_MY_NAME->vpid, node);

    /* create and store a proc object for us */
    proc = OBJ_NEW(orte_proc_t);
    proc->name.jobid = ORTE_PROC_MY_NAME->jobid;
    proc->name.vpid = ORTE_PROC_MY_NAME->vpid;
    proc->pid = orte_process_info.pid;
    proc->state = ORTE_PROC_STATE_RUNNING;
    OBJ_RETAIN(node);  /* keep accounting straight */
    proc->node = node;
    proc->nodename = node->name;
    opal_pointer_array_set_item(daemons->procs, proc->name.vpid, proc);

    /* record that the daemon (i.e., us) is on this node 
     * NOTE: we do not add the proc object to the node's
     * proc array because we are not an application proc.
     * Instead, we record it in the daemon field of the
     * node object
     */
    OBJ_RETAIN(proc);   /* keep accounting straight */
    node->daemon = proc;
    node->daemon_launched = true;
    node->state = ORTE_NODE_STATE_UP;
    
    /* record that the daemon job is running */
    daemons->num_procs = 1;
    daemons->state = ORTE_JOB_STATE_RUNNING;
    
    /* open and setup the opal_pstat framework so we can provide
     * process stats if requested
     */
    if (ORTE_SUCCESS != (ret = opal_pstat_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "opal_pstat_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = opal_pstat_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_pstat_base_select";
        goto error;
    }

    /* open and setup the local resource discovery framework */
    if (ORTE_SUCCESS != (ret = opal_sysinfo_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "opal_sysinfo_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = opal_sysinfo_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "opal_sysinfo_base_select";
        goto error;
    }
        
    /* Setup the communication infrastructure */
    
    /* Runtime Messaging Layer - this opens/selects the OOB as well */
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
    
    /* Open/select the odls */
    if (ORTE_SUCCESS != (ret = orte_odls_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_odls_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_odls_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_odls_base_select";
        goto error;
    }
    
    /* enable communication with the rml */
    if (ORTE_SUCCESS != (ret = orte_rml.enable_comm())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_rml.enable_comm";
        goto error;
    }
    
    /* insert our contact info into our process_info struct so we
     * have it for later use and set the local daemon field to our name
     */
    orte_process_info.my_daemon_uri = orte_rml.get_contact_info();
    ORTE_PROC_MY_DAEMON->jobid = ORTE_PROC_MY_NAME->jobid;
    ORTE_PROC_MY_DAEMON->vpid = ORTE_PROC_MY_NAME->vpid;
    proc->rml_uri = orte_rml.get_contact_info();

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

    /* set the communication function */
    orte_comm = orte_global_comm;
    
    /* update the routing tree */
    if (ORTE_SUCCESS != (ret = orte_routed.update_routing_tree())) {
        ORTE_ERROR_LOG(ret);
        error = "failed to update routing tree";
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
    
    /* open/select the errmgr - do this after the daemon job
     * has been defined so that the errmgr can get that
     * job object
     */
    if (ORTE_SUCCESS != (ret = orte_errmgr_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_errmgr_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_errmgr_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_errmgr_base_select";
        goto error;
    }

    /* We actually do *not* want an orcm to voluntarily yield() the
       processor more than necessary. orcm already blocks when
       it is doing nothing, so it doesn't use any more CPU cycles than
       it should; but when it *is* doing something, we do not want it
       to be unnecessarily delayed because it voluntarily yielded the
       processor in the middle of its work.
     
       For example: when a message arrives at orcm, we want the
       OS to wake us up in a timely fashion (which most OS's
       seem good about doing) and then we want orcm to process
       the message as fast as possible.  If orcm yields and lets
       aggressive applications get the processor back, it may be a
       long time before the OS schedules orcm to run again
       (particularly if there is no IO event to wake it up).  Hence,
       routed OOB messages (for example) may be significantly delayed
       before being delivered to processes, which can be
       problematic in some scenarios */
    opal_progress_set_yield_when_idle(false);

    /* Change the default behavior of libevent such that we want to
       continually block rather than blocking for the default timeout
       and then looping around the progress engine again.  There
       should be nothing in the orcmd that cannot block in libevent
       until "something" happens (i.e., there's no need to keep
       cycling through progress because the only things that should
       happen will happen in libevent).  This is a minor optimization,
       but what the heck... :-) */
    opal_progress_set_event_flag(OPAL_EVLOOP_ONCE);
    
    /* setup the primary daemon command receive function */
    ret = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD, ORTE_RML_TAG_DAEMON,
                                  ORTE_RML_NON_PERSISTENT, orte_daemon_recv, NULL);
    if (ret != ORTE_SUCCESS && ret != ORTE_ERR_NOT_IMPLEMENTED) {
        ORTE_ERROR_LOG(ret);
        error = "daemon_recv";
        goto error;
    }

    /* output a message indicating we are alive, our name, and our pid
     * for debugging purposes
     */
    if (orte_debug_daemons_flag) {
        fprintf(stderr, "%s checking in as pid %ld on host %s\n",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (long)orte_process_info.pid,
                orte_process_info.nodename);
    }
    
    /* listen for termination cmds */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcm", "0.1", "alpha",
                                                         ORCM_PNP_SYS_CHANNEL,
                                                         ORCM_PNP_TAG_TERMINATE,
                                                         vm_term, NULL))) {
        error = "orcm recv";
        goto error;
    }
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcm-stop", "0.1", "alpha",
                                                         ORCM_PNP_SYS_CHANNEL,
                                                         ORCM_PNP_TAG_TERMINATE,
                                                         vm_term, NULL))) {
        error = "orcm-stop recv";
        goto error;
    }
    /* register to catch vm commands requests */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcm-sched", "0.1", "alpha",
                                                         ORCM_PNP_SYS_CHANNEL,
                                                         ORCM_PNP_TAG_COMMAND,
                                                         vm_cmd, NULL))) {
        error = "orcm-sched recv";
        goto error;
    }
    /* listen for state data requests */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcm-sched", "0.1", "alpha",
                                                         ORCM_PNP_SYS_CHANNEL,
                                                         ORCM_PNP_TAG_BOOTSTRAP,
                                                         recv_contact, NULL))) {
        error = "contact recv";
        goto error;
    }

    /* setup the SENSOR framework */
    if (ORTE_SUCCESS != (ret = orte_sensor_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_sensor_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_sensor_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_sensor_select";
        goto error;
    }

    /* announce our existence - this carries with it our rml uri and
     * our local node system info
     */
    if (ORCM_SUCCESS != (ret = orcm_pnp.announce("ORCMD", "0.1", "alpha", vm_tracker))) {
        ORTE_ERROR_LOG(ret);
        error = "announce";
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
    int i;
    orte_node_t *node;
    orte_job_t *job;

    /* stop the local sensors */
    orte_sensor.stop(ORTE_PROC_MY_NAME->jobid);

    orte_sensor_base_close();

    orte_odls_base_close();
    
    orte_wait_finalize();

    /* finalize selected modules */
    orte_errmgr_base_close();

    /* close the orcm-related frameworks */
    orcm_leader_base_close();
    orcm_pnp_base_close();

    /* close the multicast */
    orte_rmcast_base_close();
    orte_routed_base_close();
    orte_rml_base_close();

    /* cleanup the job and node info arrays */
    if (NULL != orte_node_pool) {
        for (i=0; i < orte_node_pool->size; i++) {
            if (NULL != (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool,i))) {
                OBJ_RELEASE(node);
            }
        }
        OBJ_RELEASE(orte_node_pool);
    }
    if (NULL != orte_job_data) {
        for (i=0; i < orte_job_data->size; i++) {
            if (NULL != (job = (orte_job_t*)opal_pointer_array_get_item(orte_job_data,i))) {
                OBJ_RELEASE(job);
            }
        }
        OBJ_RELEASE(orte_job_data);
    }

    /* handle the orcm-specific OPAL stuff */
    opal_sysinfo_base_close();
    opal_pstat_base_close();

}

static void vm_tracker(orcm_info_t *vm)
{
    orte_proc_t *proc;
    orte_node_t *node;
    int i;
    uint8_t trig=0;

    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s Received announcement from %s:%s:%s proc %s on node %s pid %lu",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), vm->app, vm->version, vm->release,
                         ORTE_NAME_PRINT(vm->name), vm->nodename, (unsigned long)vm->pid));
    
    /* if this isn't one of my peers, ignore it */
    if (vm->name->jobid != ORTE_PROC_MY_NAME->jobid) {
        /* if this is an orcm or orcmd that belongs to this user, then we have a problem */
        if ((0 == strcasecmp(vm->app, "orcmd") || (0 == strcasecmp(vm->app, "orcm"))) && vm->uid == my_uid) {
            orte_show_help("help-orcm.txt", "preexisting-orcmd", true, vm->nodename);
        }
        return;
    }

    /* look up this proc */
    if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(daemons->procs, vm->name->vpid))) {
        OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                             "%s adding new daemon %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(vm->name)));
        
        /* new daemon - add it */
        proc = OBJ_NEW(orte_proc_t);
        proc->name.jobid = vm->name->jobid;
        proc->name.vpid = vm->name->vpid;
        proc->rml_uri = strdup(vm->rml_uri);
        daemons->num_procs++;
        opal_pointer_array_set_item(daemons->procs, vm->name->vpid, proc);
        /* update our num_procs */
        orte_process_info.num_procs = daemons->num_procs;
    } else {
        /* this daemon must have restarted - see if I am the one
         * responsible with bringing him up to date
         */
        if (ORTE_PROC_MY_NAME->vpid == orte_get_lowest_vpid_alive(ORTE_PROC_MY_NAME->jobid)) {
            /* falls to me - collect all the job and map info */
            /* send it direct to the replacement */
        }
    }
    /* update the pid, in case it changed */
    proc->pid = vm->pid;

    /* update the rml_uri if required */
    if (NULL == proc->rml_uri) {
        proc->rml_uri = strdup(vm->rml_uri);
    } else if (0 != strcmp(proc->rml_uri, vm->rml_uri)) {
        free(proc->rml_uri);
        proc->rml_uri = strdup(vm->rml_uri);
    }
    /* ensure the state is set to running */
    proc->state = ORTE_PROC_STATE_RUNNING;
    /* exit code is obviously zero */
    proc->exit_code = 0;

    /* get the node - it is at the index of the daemon's vpid */
    if (NULL != (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, vm->name->vpid))) {
        /* already have this node - could be a race condition
         * where the daemon died and has been replaced, so
         * just assume that is the case
         */
        if (NULL != node->daemon) {
            OBJ_RELEASE(node->daemon);
        }
        node->state = ORTE_NODE_STATE_UP;
        goto complete;
    }
    /* if we get here, this is a new node */
    node = OBJ_NEW(orte_node_t);
    node->name = strdup(vm->nodename);
    node->state = ORTE_NODE_STATE_UP;
    node->slots = 1;  /* min number */
    node->slots_alloc = node->slots;
    node->index = vm->name->vpid;
    opal_pointer_array_set_item(orte_node_pool, vm->name->vpid, node);
 complete:
    OBJ_RETAIN(node);  /* maintain accounting */
    proc->node = node;
    proc->nodename = node->name;
    OBJ_RETAIN(proc);  /* maintain accounting */
    node->daemon = proc;
    node->daemon_launched = true;

    return;
}

static void vm_term(int status,
                    orte_process_name_t *sender,
                    orcm_pnp_tag_t tag,
                    struct iovec *msg, int count,
                    opal_buffer_t *buf,
                    void *cbdata)
{
    int rc, n;
    uint16_t jfam;
    opal_buffer_t response;
    orcm_tool_cmd_t flag=ORCM_TOOL_STOP_CMD;

    OPAL_OUTPUT_VERBOSE((1, orte_ess_base_output,
                         "%s GOT TERM COMMAND FROM %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));

    /* if this isn't intended for me, ignore it */
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &jfam, &n, OPAL_UINT16))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    if (jfam != ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid)) {
        OPAL_OUTPUT_VERBOSE((1, orte_ess_base_output,
                             "%s GOT TERM COMMAND FOR DVM %d - NOT FOR ME!",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), jfam));
        return;
    }

    ORTE_TIMER_EVENT(0, 0, orcm_just_quit);
}

static void vm_cmd(int status,
                   orte_process_name_t *sender,
                   orcm_pnp_tag_t tag,
                   struct iovec *msg,
                   int count,
                   opal_buffer_t *buffer,
                   void *cbdata)
{
    int rc, n;
    uint16_t jfam;
    orte_process_name_t generator;

    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s GOT COMMAND FROM %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));

    /* if this isn't intended for me, ignore it */
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &jfam, &n, OPAL_UINT16))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    if (jfam != ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid)) {
        OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                             "%s GOT COMMAND FOR DVM %d - NOT FOR ME!",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), jfam));
        return;
    }

    ORTE_MESSAGE_EVENT(ORTE_PROC_MY_NAME, buffer, ORTE_RML_TAG_DAEMON, orte_daemon_cmd_processor);
}

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

static void recv_contact(int status,
                         orte_process_name_t *peer,
                         orcm_pnp_tag_t tag,
                         struct iovec *msg,
                         int count,
                         opal_buffer_t *buffer,
                         void *cbdata)
{
    orte_process_name_t name;
    orte_daemon_cmd_flag_t command;
    int n, rc;
    int32_t num;
    opal_buffer_t *ans;
    opal_list_item_t *item;
    orte_odls_job_t *jobdat;
    orte_odls_child_t *child;
    orte_app_context_t *app;
    orte_app_idx_t na;

    /* is this responding to an announce from me? */
    n = 1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &name, &n, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    if (name.jobid != ORTE_PROC_MY_NAME->jobid ||
        name.vpid != ORTE_PROC_MY_NAME->vpid) {
        /* nope - someone else */
        OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                             "%s GOT SCHED CONTACT COMMAND FOR %s - IGNORING",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&name)));
        /* DO NOT send a response */
        return;
    }

    OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                         "%s GOT SCHED CONTACT COMMAND",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));

    /* identify the sending scheduler as my HNP */
    ORTE_PROC_MY_HNP->jobid = peer->jobid;
    ORTE_PROC_MY_HNP->vpid = peer->vpid;

    /* start the local sensors - do this now so heartbeats don't
     * start running before the scheduler knows we exist
     */
    orte_sensor.start(ORTE_PROC_MY_NAME->jobid);

    /* prep return */
    ans = OBJ_NEW(opal_buffer_t);

    /* get the command */
    n = 1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &command, &n, ORTE_DAEMON_CMD_T))) {
        ORTE_ERROR_LOG(rc);
        goto release;
    }

    switch(command) {
    case ORTE_DAEMON_NULL_CMD:
        OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                             "%s ACK FROM SCHED",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        OBJ_RELEASE(ans);
        return;

    case ORTE_DAEMON_KILL_LOCAL_PROCS:
        OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                             "%s KILLING LOCAL PROCS BY SCHED COMMAND",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        rc = orte_odls.kill_local_procs(NULL);
        opal_dss.pack(ans, &rc, 1, OPAL_INT);
        break;

    case ORTE_DAEMON_CHECKIN_CMD:
        OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                             "%s RETURNING CURRENT STATE BY SCHED COMMAND",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        rc = ORTE_SUCCESS;
        opal_dss.pack(ans, &rc, 1, OPAL_INT);
        /* return the number of local job entries */
        num = opal_list_get_size(&orte_local_jobdata);
        opal_dss.pack(ans, &num, 1, OPAL_INT32);
        /* now pass the data for each one */
        for (item = opal_list_get_first(&orte_local_jobdata);
             item != opal_list_get_end(&orte_local_jobdata);
             item = opal_list_get_next(item)) {
            jobdat = (orte_odls_job_t*)item;
            /* send jobid */
            opal_dss.pack(ans, &jobdat->jobid, 1, ORTE_JOBID);
            /* send instance and name */
            opal_dss.pack(ans, &jobdat->instance, 1, OPAL_STRING);
            opal_dss.pack(ans, &jobdat->name, 1, OPAL_STRING);
            /* send app contexts */
            opal_dss.pack(ans, &jobdat->num_apps, 1, ORTE_APP_IDX);
            for (na=0; na < jobdat->num_apps; na++) {
                opal_dss.pack(ans, &jobdat->apps[na], 1, ORTE_APP_CONTEXT);
            }
        }
        /* return the number of local children entries */
        num = opal_list_get_size(&orte_local_children);
        opal_dss.pack(ans, &num, 1, OPAL_INT32);
        /* pass their data */
        for (item = opal_list_get_first(&orte_local_children);
             item != opal_list_get_end(&orte_local_children);
             item = opal_list_get_next(item)) {
            child = (orte_odls_child_t*)item;
            /* send name */
            opal_dss.pack(ans, child->name, 1, ORTE_NAME);
            /* send app index */
            opal_dss.pack(ans, &child->app_idx, 1, ORTE_APP_IDX);
            /* send pid */
            opal_dss.pack(ans, &child->pid, 1, OPAL_PID);
            /* send state */
            opal_dss.pack(ans, &child->state, 1, ORTE_PROC_STATE);
            /* send incarnation */
            opal_dss.pack(ans, &child->restarts, 1, OPAL_INT32);
        }
        break;

    default:
        OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                             "%s UNRECOGNIZED CONTACT COMMAND FROM %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(peer)));
        rc = ORTE_ERR_BAD_PARAM;
        opal_dss.pack(ans, &rc, 1, OPAL_INT);
        break;
    }

 release:
    if (ORTE_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_SYS_CHANNEL,
                                                 NULL, ORCM_PNP_TAG_BOOTSTRAP,
                                                 NULL, 0, ans, cbfunc, NULL))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(ans);
    }
}

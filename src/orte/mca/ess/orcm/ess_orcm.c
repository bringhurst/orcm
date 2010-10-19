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

#include "opal/util/argv.h"
#include "opal/util/if.h"
#include "opal/util/opal_sos.h"
#include "opal/util/os_path.h"
#include "opal/mca/paffinity/paffinity.h"
#include "opal/mca/sysinfo/sysinfo.h"
#include "opal/mca/sysinfo/base/base.h"
#include "opal/threads/mutex.h"
#include "opal/threads/condition.h"

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
#include "orte/util/nidmap.h"
#include "orte/runtime/orte_wait.h"
#include "orte/runtime/orte_globals.h"
#include "orte/orted/orted.h"

#include "orte/mca/ess/ess.h"
#include "orte/mca/ess/base/base.h"
#include "orte/mca/ess/orcm/ess_orcm.h"

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


orte_ess_base_module_t orte_ess_orcm_module = {
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
static bool start_flag;
static opal_mutex_t start_lock;
static opal_condition_t start_cond;
static char *log_path = NULL;
static uint32_t my_uid;

static void local_fin(void);
static int local_setup(char **hosts);
static void vm_tracker(char *app, char *version, char *release,
                       orte_process_name_t *name, char *node,
                       char *rml_uri, uint32_t uid);
static void ps_request(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buffer,
                       void *cbdata);
static void release(int fd, short flag, void *dump);
static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buf,
                       void *cbdata);


static int rte_init(void)
{
    int ret;
    char *error = NULL;
    char **hosts = NULL;
    char *nodelist;
    char *tmp=NULL;
    orte_jobid_t jobid=ORTE_JOBID_INVALID;
    orte_vpid_t vpid=ORTE_VPID_INVALID;
    int32_t jfam;

    OBJ_CONSTRUCT(&start_lock, opal_mutex_t);
    OBJ_CONSTRUCT(&start_cond, opal_condition_t);
    start_flag = true;
    
    my_uid = (uint32_t)getuid();

    /* run the prolog */
    if (ORTE_SUCCESS != (ret = orte_ess_base_std_prolog())) {
        error = "orte_ess_base_std_prolog";
        goto error;
    }
    
    /* open the plm in case we need it to set out name */
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
    if (ORTE_JOBID_INVALID != jobid &&
        ORTE_VPID_INVALID != vpid) {
        goto complete;
    }
        
    /* if we were given an HNP, we can get the jobid from
     * the HNP's name - this is decoded in proc_info.c during
     * the prolog
     */
    if (ORTE_JOBID_INVALID != ORTE_PROC_MY_HNP->jobid) {
        ORTE_PROC_MY_NAME->jobid = orte_process_info.my_hnp.jobid;
        /* get vpid from environ */
        mca_base_param_reg_string_name("orte", "ess_vpid", "Process vpid",
                                       true, false, NULL, &tmp);
        if (NULL != tmp) {
            if (ORTE_SUCCESS != (ret = orte_util_convert_string_to_vpid(&vpid, tmp))) {
                error = "convert_string_to_vpid";
                goto error;
            }
            free(tmp);
            ORTE_PROC_MY_NAME->vpid = vpid;
            goto complete;
        }
    }
        
    /* if we were given a job family to join, get it */
    mca_base_param_reg_string_name("orte", "ess_job_family", "Job family",
                                   true, false, NULL, &tmp);
    if (NULL != tmp) {
        jfam = strtol(tmp, NULL, 10);
        ORTE_PROC_MY_NAME->jobid = ORTE_CONSTRUCT_JOB_FAMILY(jfam);
        goto complete;
    }

    /* otherwise, create our own name */
    if (ORTE_SUCCESS != (ret = orte_plm.set_hnp_name())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_plm_set_hnp_name";
        goto error;
    }

 complete:
    /* get the list of nodes used for this job */
    nodelist = getenv("OMPI_MCA_orte_nodelist");
        
    if (NULL != nodelist) {
        /* split the node list into an argv array */
        hosts = opal_argv_split(nodelist, ',');
    }
    if (ORTE_SUCCESS != (ret = local_setup(hosts))) {
        ORTE_ERROR_LOG(ret);
        error = "local_setup";
        goto error;
    }
    opal_argv_free(hosts);

    OBJ_DESTRUCT(&start_lock);
    OBJ_DESTRUCT(&start_cond);
    
    return ORTE_SUCCESS;
    
 error:
    orte_show_help("help-orte-runtime.txt",
                   "orte_init:startup:internal-failure",
                   true, error, ORTE_ERROR_NAME(ret), ret);
    
    OBJ_DESTRUCT(&start_lock);
    OBJ_DESTRUCT(&start_cond);
    
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


static int local_setup(char **hosts)
{
    int ret;
    char *error = NULL;
    int value;
    orte_proc_t *proc;
    orte_node_t *node;
    char log_file[PATH_MAX];
    char *jobidstring;
    int fd;
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
    
    /*
     * Group communications
     */
    if (ORTE_SUCCESS != (ret = orte_grpcomm_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_grpcomm_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_grpcomm_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_grpcomm_base_select";
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
    
    /* Setup the remaining resource management frameworks */
    if (ORTE_SUCCESS != (ret = orte_ras_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_ras_base_open";
        goto error;
    }
    
    if (ORTE_SUCCESS != (ret = orte_ras_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_ras_base_find_available";
        goto error;
    }
    
    if (ORTE_SUCCESS != (ret = orte_rmaps_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_rmaps_base_open";
        goto error;
    }
    
    if (ORTE_SUCCESS != (ret = orte_rmaps_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_rmaps_base_find_available";
        goto error;
    }
    
    /* initialize the nidmaps */
    if (ORTE_SUCCESS != (ret = orte_util_nidmap_init(NULL))) {
        ORTE_ERROR_LOG(ret);
        error = "orte_util_nidmap_init";
        goto error;
    }
    /* setup our entry */
    if (ORTE_SUCCESS != (ret = orte_util_setup_local_nidmap_entries())) {
        ORTE_ERROR_LOG(ret);
        error = "setup_local_nidmap_entries";
        goto error;
    }

    /* be sure to update the routing tree so the initial "phone home"
     * to mpirun goes through the tree!
     */
    if (ORTE_SUCCESS != (ret = orte_routed.update_routing_tree())) {
        ORTE_ERROR_LOG(ret);
        error = "failed to update routing tree";
        goto error;
    }

    /* Now provide a chance for the PLM
     * to perform any module-specific init functions. This
     * needs to occur AFTER the communications are setup
     * as it may involve starting a non-blocking recv
     * Do this only if a specific PLM was given to us - the
     * orted has no need of the proxy PLM at all
     */
    if (ORTE_SUCCESS != (ret = orte_plm.init())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_plm_init";
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
    
    /* setup I/O forwarding system - must come after we init routes */
    if (ORTE_SUCCESS != (ret = orte_iof_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_iof_base_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_iof_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_iof_base_select";
        goto error;
    }
    
    /* setup the notifier system */
    if (ORTE_SUCCESS != (ret = orte_notifier_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_notifer_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_notifier_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_notifer_select";
        goto error;
    }
    
    /* setup the db framework */
    if (ORTE_SUCCESS != (ret = orte_db_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_db_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_db_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_db_select";
        goto error;
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
    if (ORTE_PROC_IS_DAEMON) {
        app->app = strdup("orcmd");
        app->name = strdup("ORCM DAEMON");
        opal_argv_append_nosize(&app->argv, "orcmd");
    } else {
        app->app = strdup("orcm");
        app->name = strdup("ORCM FRONTEND");
        opal_argv_append_nosize(&app->argv, "orcm");
    }
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
    node->index = opal_pointer_array_set_item(orte_node_pool, ORTE_PROC_MY_NAME->vpid, node);

    /* create and store a proc object for us */
    proc = OBJ_NEW(orte_proc_t);
    proc->name.jobid = ORTE_PROC_MY_NAME->jobid;
    proc->name.vpid = ORTE_PROC_MY_NAME->vpid;
    proc->pid = orte_process_info.pid;
    proc->rml_uri = orte_rml.get_contact_info();
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

    /* setup the show help recv */
    if (ORCM_PROC_IS_MASTER) {
        ret = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD, ORTE_RML_TAG_SHOW_HELP,
                                      ORTE_RML_NON_PERSISTENT, orte_show_help_recv, NULL);
        if (ret != ORTE_SUCCESS && ret != ORTE_ERR_NOT_IMPLEMENTED) {
            ORTE_ERROR_LOG(ret);
            error = "show_help_recv";
            goto error;
        }
    }


    /* output a message indicating we are alive, our name, and our pid
     * for debugging purposes
     */
    if (orte_debug_daemons_flag) {
        fprintf(stderr, "%s checking in as pid %ld on host %s\n",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (long)orte_process_info.pid,
                orte_process_info.nodename);
    }
    
    /* setup stdout/stderr */
    if (orte_debug_daemons_file_flag) {
        /* if we are debugging to a file, then send stdout/stderr to
         * the orcmd log file
         */
        
        /* get my jobid */
        if (ORTE_SUCCESS != (ret = orte_util_convert_jobid_to_string(&jobidstring,
                                                                     ORTE_PROC_MY_NAME->jobid))) {
            ORTE_ERROR_LOG(ret);
            error = "convert_jobid_to_string";
            goto error;
        }
        
        /* define a log file name in the session directory */
        snprintf(log_file, PATH_MAX, "output-orcmd-%s-%s.log",
                 jobidstring, orte_process_info.nodename);
        log_path = opal_os_path(false,
                                orte_process_info.tmpdir_base,
                                orte_process_info.top_session_dir,
                                log_file,
                                NULL);
        
        fd = open(log_path, O_RDWR|O_CREAT|O_TRUNC, 0640);
        if (fd < 0) {
            /* couldn't open the file for some reason, so
             * just connect everything to /dev/null
             */
            fd = open("/dev/null", O_RDWR|O_CREAT|O_TRUNC, 0666);
        } else {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if(fd != STDOUT_FILENO && fd != STDERR_FILENO) {
                close(fd);
            }
        }
    }
    
    /* if we are a daemon, listen for cmds */
    if (ORCM_PROC_IS_DAEMON) {
        if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcm", "0.1", "alpha",
                                                             ORCM_PNP_SYS_CHANNEL,
                                                             ORCM_PNP_TAG_WILDCARD,
                                                             recv_input))) {
            ORTE_ERROR_LOG(ret);
            orte_quit();
        }
    }

    /* listen for PS requests */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcm-ps", "0.1", "alpha",
                                                         ORCM_PNP_SYS_CHANNEL,
                                                         ORCM_PNP_TAG_PS,
                                                         ps_request))) {
        ORTE_ERROR_LOG(ret);
        orte_quit();
    }
    
    if (orte_debug_daemons_flag) {
        opal_output(0, "%s up and running - waiting for commands!", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    }

    /* announce our existence - this carries with it our rml uri and
     * our local node system info
     */
    if (ORCM_PROC_IS_DAEMON) {
        if (ORCM_SUCCESS != (ret = orcm_pnp.announce("ORCMD", "0.1", "alpha", vm_tracker))) {
            ORTE_ERROR_LOG(ret);
            error = "announce";
            goto error;
        }
    } else {
        if (ORCM_SUCCESS != (ret = orcm_pnp.announce("ORCM", "0.1", "alpha", vm_tracker))) {
            ORTE_ERROR_LOG(ret);
            error = "announce";
            goto error;
        }
    }

    /* give ourselves a second to wait for announce responses to detect
     * any pre-existing daemons/orcms that might conflict
     */
    if (ORCM_PROC_IS_MASTER) {
        ORTE_TIMER_EVENT(1, 0, release);
    
        OPAL_ACQUIRE_THREAD(&start_lock, &start_cond, &start_flag);
        OPAL_THREAD_UNLOCK(&start_lock);
    }

    /* if we are an orcmd, open the cfgi framework so
     * we can receive configuration instructions
     */
    if (ORCM_PROC_IS_DAEMON) {
        if (ORTE_SUCCESS != (ret = orcm_cfgi_base_open())) {
            ORTE_ERROR_LOG(ret);
            error = "orcm_cfgi_open";
            goto error;
        }
        if (ORTE_SUCCESS != (ret = orcm_cfgi_base_select())) {
            ORTE_ERROR_LOG(ret);
            error = "orcm_cfgi_select";
            goto error;
        }
    }

    /* setup the SENSOR framework */
    if (ORTE_SUCCESS != (ret = orte_sensor_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_sensor_open";
        goto error;
    }
    if (ORTE_SUCCESS != (ret = orte_sensor_base_select())) {
        ORTE_ERROR_LOG(ret);
        error = "ortesensor_select";
        goto error;
    }
    /* start the local sensors - do this last so heartbeats don't
     * start running too early
     */
    orte_sensor.start(ORTE_PROC_MY_NAME->jobid);

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

     /* cleanup */
    if (NULL != log_path) {
        unlink(log_path);
    }
    
   /* stop the local sensors */
    orte_sensor.stop(ORTE_PROC_MY_NAME->jobid);

    orte_sensor_base_close();
    orte_db_base_close();
    orte_notifier_base_close();
    
    orte_odls_base_close();
    
    orte_wait_finalize();
    orte_iof_base_close();

    /* finalize selected modules */
    orte_ras_base_close();
    orte_rmaps_base_close();
    orte_plm_base_close();
    orte_errmgr_base_close();

    /* close the orcm-related frameworks */
    orcm_leader_base_close();
    if (ORCM_PROC_IS_DAEMON) {
        orcm_cfgi_base_close();
    }
    orcm_pnp_base_close();

    /* now can close the rml and its friendly group comm */
    orte_grpcomm_base_close();
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

static void vm_tracker(char *app, char *version, char *release,
                       orte_process_name_t *name, char *nodename,
                       char *rml_uri, uint32_t uid)
{
    orte_proc_t *proc;
    orte_node_t *node;
    orte_nid_t *nid;
    int i;
    
    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s Received announcement from %s:%s:%s proc %s on node %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), app, version, release,
                         ORTE_NAME_PRINT(name), nodename));
    
    /* if this isn't one of my peers, ignore it */
    if (name->jobid != ORTE_PROC_MY_NAME->jobid) {
        /* if this is an orcm or orcmd that belongs to this user, then we have a problem */
        if ((0 == strcasecmp(app, "orcmd") || (0 == strcasecmp(app, "orcm"))) && uid == my_uid) {
            orte_show_help("help-orcm.txt", "preexisting-orcmd", true, nodename);
            goto exitout;
        }
        return;
    }
    
    /* look up this proc */
    if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(daemons->procs, name->vpid))) {
        OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                             "%s adding new daemon %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(name)));
        
        /* new daemon - add it */
        proc = OBJ_NEW(orte_proc_t);
        proc->name.jobid = name->jobid;
        proc->name.vpid = name->vpid;
        proc->rml_uri = strdup(rml_uri);
        daemons->num_procs++;
        opal_pointer_array_set_item(daemons->procs, name->vpid, proc);
        /* if we are a daemon, update our num_procs */
        if (ORCM_PROC_IS_DAEMON) {
            orte_process_info.num_procs++;
        }
    }
    /* update the rml_uri if required */
    if (NULL == proc->rml_uri) {
        proc->rml_uri = strdup(rml_uri);
    } else if (0 != strcmp(proc->rml_uri, rml_uri)) {
        free(proc->rml_uri);
        proc->rml_uri = strdup(rml_uri);
    }
    /* ensure the state is set to running */
    proc->state = ORTE_PROC_STATE_RUNNING;
    /* exit code is obviously zero */
    proc->exit_code = 0;

    /* get the node - it is at the index of the daemon's vpid */
    if (NULL != (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, name->vpid))) {
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
    node->name = strdup(nodename);
    node->state = ORTE_NODE_STATE_UP;
    node->slots = 1;  /* min number */
    node->slots_alloc = node->slots;
    node->index = opal_pointer_array_set_item(orte_node_pool, name->vpid, node);
 complete:
    OBJ_RETAIN(node);  /* maintain accounting */
    proc->node = node;
    proc->nodename = node->name;
    OBJ_RETAIN(proc);  /* maintain accounting */
    node->daemon = proc;
    node->daemon_launched = true;
    if (ORCM_PROC_IS_MASTER) {
        daemons->num_reported++;
    }

    /* see if we need to update the nidmap */
    if (NULL == (nid = (orte_nid_t*)orte_util_lookup_nid(name))) {
        OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                             "%s adding new nid for %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(name)));
        
        nid = OBJ_NEW(orte_nid_t);
        nid->daemon = name->vpid;
        nid->name = strdup(nodename);
        nid->index = opal_pointer_array_add(&orte_nidmap, nid);
    } else {
        /* already exists - see if we need to update it */
        if (NULL == nid->name) {
            nid->name = strdup(nodename);
        } else if (0 != strcmp(nodename, nid->name)) {
            free(nid->name);
            nid->name = strdup(nodename);
        }
    }

    /* if it is a restart, check the node against the
     * new one to see if it changed location
     */
    if (NULL != proc->nodename) {
        if (0 != strcmp(nodename, proc->nodename)) {
            OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                                 "%s restart detected",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
            /* must have moved */
            OBJ_RELEASE(proc->node);  /* maintain accounting */
            proc->nodename = NULL;
        }
    }

    if (ORCM_PROC_IS_MASTER) {
        OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                             "%s %d of %d reported",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             daemons->num_reported, daemons->num_procs));

        /* check if we have heard from them all */
        if (daemons->num_procs <= daemons->num_reported) {
            OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                                 "%s declaring launch complete",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        
            OPAL_WAKEUP_THREAD(&orte_plm_globals.spawn_in_progress_cond,
                               &orte_plm_globals.spawn_in_progress);
        }
    }
    
    return;
    
 exitout:
    /* kill any local procs */
    orte_odls.kill_local_procs(NULL);

    /* whack any lingering session directory files from our jobs */
    orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
    
    /* cleanup and leave */
    orcm_finalize();
    
    exit(orte_exit_status);
}

static void release(int fd, short flag, void *dump)
{
    OPAL_WAKEUP_THREAD(&start_cond, &start_flag);
}

static void ps_request(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buffer,
                       void *cbdata)
{
    orte_process_name_t name;
    int32_t n;
    int rc;
    opal_buffer_t ans;
    
    /* unpack the target name */
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &name, &n, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* construct the response */
    OBJ_CONSTRUCT(&ans, opal_buffer_t);
    
    /* if the jobid is wildcard, they just want to know who is out there */
    if (ORTE_JOBID_WILDCARD == name.jobid) {
        goto respond;
    }
    
    /* if the requested job family isn't mine, and isn't my DVM, then ignore it */
    if (ORTE_JOB_FAMILY(name.jobid) != ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid)) {
        OBJ_DESTRUCT(&ans);
        return;
    }
    
    /* if the request is for my job family... */
    if (ORTE_JOB_FAMILY(name.jobid) == ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid)) {
        /* if the vpid is wildcard, then the caller wants this info from everyone.
         * if the vpid is not wildcard, then only respond if I am the leader
         */
        if (ORTE_VPID_WILDCARD == name.vpid) {
            /* pack the response */
            goto pack;
        }
        /* otherwise, just ignore this */
        OBJ_DESTRUCT(&ans);
        return;
    }
    
    /* the request must have been for my DVM - if they don't want everyone to
     * respond, ignore it
     */
    if (ORTE_VPID_WILDCARD != name.vpid) {
        OBJ_DESTRUCT(&ans);
        return;
    }
    
 pack:
    opal_dss.pack(&ans, ORTE_PROC_MY_NAME, 1, ORTE_NAME);
    
 respond:
    if (ORCM_SUCCESS != (rc = orcm_pnp.output(ORCM_PNP_SYS_CHANNEL,
                                              NULL, ORCM_PNP_TAG_PS,
                                              NULL, 0, &ans))) {
        ORTE_ERROR_LOG(rc);
    }
    OBJ_DESTRUCT(&ans);
}

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buf,
                       void *cbdata)
{
    int rc, n;
    uint16_t jfam;
    ptrdiff_t unpack_rel, save_rel;
    orte_daemon_cmd_flag_t command;

    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s GOT COMMAND FROM %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));

    /* if this isn't intended for me, ignore it */
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &jfam, &n, OPAL_UINT16))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    if (jfam != ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid)) {
        OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                             "%s GOT COMMAND FOR DVM %d - NOT FOR ME!",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), jfam));
        return;
    }
    
    /* save the original buffer pointers */
    unpack_rel = buf->unpack_ptr - buf->base_ptr;
    
    /* unpack the initial command */
    n = 1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &command, &n, ORTE_DAEMON_CMD))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    /* if it is a halt command, then handle it here as we have additional
     * things to do than orte would know about
     */
    if (ORTE_DAEMON_HALT_VM_CMD == command) {
        /* kill any local procs */
        orte_odls.kill_local_procs(NULL);

        /* whack any lingering session directory files from our jobs */
        orte_session_dir_cleanup(ORTE_JOBID_WILDCARD);
    
        /* cleanup and leave */
        orcm_finalize();
        exit(orte_exit_status);
    }

    /* rewind the buffer to the right place for processing the cmd */
    buf->unpack_ptr = buf->base_ptr + save_rel;

    /* pass it down to the cmd processor */
    if (ORTE_SUCCESS != (rc = orte_daemon_process_commands(sender, buf, tag))) {
        OPAL_OUTPUT_VERBOSE((1, orte_ess_base_output,
                             "%s orte:daemon:cmd:processor failed on error %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_ERROR_NAME(rc)));
    }
}


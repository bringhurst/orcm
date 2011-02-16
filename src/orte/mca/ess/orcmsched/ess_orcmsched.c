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

#include "opal/util/argv.h"
#include "opal/util/if.h"
#include "opal/util/os_path.h"
#include "opal/mca/paffinity/paffinity.h"
#include "opal/mca/sysinfo/sysinfo.h"
#include "opal/mca/sysinfo/base/base.h"
#include "opal/mca/event/event.h"

#include "orte/threads/threads.h"
#include "orte/mca/rmcast/base/base.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/plm/base/base.h"
#include "orte/mca/plm/base/plm_private.h"
#include "orte/mca/rmaps/rmaps_types.h"
#include "orte/mca/rml/base/base.h"
#include "orte/mca/rml/base/rml_contact.h"
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
#include "orte/mca/ess/orcmsched/ess_orcmsched.h"

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


orte_ess_base_module_t orte_ess_orcmsched_module = {
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
static orte_app_context_t *daemon_app;
static orte_thread_ctl_t ctl, local_ctl;
static uint32_t my_uid;
static opal_event_t timeout;
static struct timeval timeout_tv = {3,0};
static opal_event_t process_ev;
static int process_pipe[2];
static bool bootstrap_complete = false;
static bool clean_startup;

static void local_fin(void);
static int local_setup(char **hosts);
static void vm_tracker(orcm_info_t *vm);
static void release(int fd, short flag, void *dump);
static void process_daemon(int fd, short flag, void *dump);
static void ps_request(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buffer,
                       void *cbdata);
static void vm_term(int status,
                    orte_process_name_t *sender,
                    orcm_pnp_tag_t tag,
                    struct iovec *msg, int count,
                    opal_buffer_t *buf,
                    void *cbdata);
static void process_contact(int status,
                            orte_process_name_t *peer,
                            orcm_pnp_tag_t tag,
                            struct iovec *msg, int count,
                            opal_buffer_t *buffer,
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

    OBJ_CONSTRUCT(&ctl, orte_thread_ctl_t);
    OBJ_CONSTRUCT(&local_ctl, orte_thread_ctl_t);

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
    }
    /* if we were given a job family to join, get it */
    mca_base_param_reg_string_name("orte", "ess_job_family", "Job family",
                                   true, false, NULL, &tmp);
    if (NULL != tmp) {
        jfam = strtol(tmp, NULL, 10);
        opal_output(0, "GOT JOB FAM OF %d", jfam);
        ORTE_PROC_MY_NAME->jobid = ORTE_CONSTRUCT_LOCAL_JOBID(jfam << 16, 0);
        /* assume a vpid of 1 - it can be overwritten later */
        ORTE_PROC_MY_NAME->vpid = 1;
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
    /* if we were able to get both, then we are done */
    if (ORTE_JOBID_INVALID != ORTE_PROC_MY_NAME->jobid &&
        ORTE_VPID_INVALID != ORTE_PROC_MY_NAME->vpid) {
        goto complete;
    }

    /* if we were given an HNP, we can get the job family from
     * the HNP's name and set ourselves to vpid=1
     */
    if (NULL != orte_process_info.my_hnp_uri) {
        /* extract the hnp name and store it */
        if (ORTE_SUCCESS != (ret = orte_rml_base_parse_uris(orte_process_info.my_hnp_uri,
                                                           ORTE_PROC_MY_HNP, NULL))) {
            ORTE_ERROR_LOG(ret);
            error = "extracting_hnp_name";
            goto error;
        }
        ORTE_PROC_MY_NAME->jobid = ORTE_PROC_MY_HNP->jobid;
        ORTE_PROC_MY_NAME->vpid = 1;
        goto complete;
    }
        
#if HAVE_QINFO_H
    /* if we have qlib, then we can ask it for info by which we determine our
     * name based on provided rack location info
     */
    {
        qinfo_t *qinfo;

        if (NULL != (qinfo = get_qinfo())) {
            /* the scheduler is always 0,1 */
            ORTE_PROC_MY_NAME->jobid = 0;
            ORTE_PROC_MY_NAME->vpid = 1;
            /* point the HNP to the zero vpid */
            ORTE_PROC_MY_HNP->jobid = 0;
            ORTE_PROC_MY_HNP->vpid = 0;
            OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                                 "GOT NAME %s FROM QINFO rack %d slot %d ",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 qinfo->rack, qinfo->slot));
            goto complete;
        }
    }
#endif

    /* otherwise, it's an error */
    error = "no_name_given";
    goto error;

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

    OBJ_DESTRUCT(&ctl);

    return ORTE_SUCCESS;
    
 error:
    orte_show_help("help-orte-runtime.txt",
                   "orte_init:startup:internal-failure",
                   true, error, ORTE_ERROR_NAME(ret), ret);
    
    OBJ_DESTRUCT(&ctl);
    OBJ_DESTRUCT(&local_ctl);
    
    return ret;
}

static int rte_finalize(void)
{
    local_fin();
    OBJ_DESTRUCT(&local_ctl);

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
    
    ORTE_ACQUIRE_THREAD(&local_ctl);

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
            ORTE_RELEASE_THREAD(&local_ctl);
            return (OPAL_PROC_ON_NODE | OPAL_PROC_ON_CU | OPAL_PROC_ON_CLUSTER);
        }
    }
    
    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s ess:orcm: proc %s is REMOTE",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(proc)));
    
    ORTE_RELEASE_THREAD(&local_ctl);
    return OPAL_PROC_NON_LOCAL;
    
}

static orte_proc_t* find_proc(orte_process_name_t *proc)
{
    orte_job_t *jdata;
    orte_proc_t *pptr;

    ORTE_ACQUIRE_THREAD(&local_ctl);

    if (NULL == (jdata = orte_get_job_data_object(proc->jobid))) {
        opal_output(0, "CANNOT FIND JOB OBJECT FOR PROC %s", ORTE_NAME_PRINT(proc));
        ORTE_RELEASE_THREAD(&local_ctl);
        return NULL;
    }

    pptr = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, proc->vpid);
    ORTE_RELEASE_THREAD(&local_ctl);
    return pptr;
}


static orte_vpid_t proc_get_daemon(orte_process_name_t *proc)
{
    orte_proc_t *pdata;
    
    ORTE_ACQUIRE_THREAD(&local_ctl);

    if( ORTE_JOBID_IS_DAEMON(proc->jobid) ) {
        ORTE_RELEASE_THREAD(&local_ctl);
        return proc->vpid;
    }

    /* get the job data */
    if (NULL == (pdata = find_proc(proc))) {
        ORTE_RELEASE_THREAD(&local_ctl);
        return ORTE_VPID_INVALID;
    }
     
    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s ess:orcm: proc %s is hosted by daemon %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(proc),
                         ORTE_VPID_PRINT(pdata->node->daemon->name.vpid)));
    
    ORTE_RELEASE_THREAD(&local_ctl);
    return pdata->node->daemon->name.vpid;
}

static char* proc_get_hostname(orte_process_name_t *proc)
{
    orte_proc_t *pdata;
    
    ORTE_ACQUIRE_THREAD(&local_ctl);

    if (NULL == (pdata = find_proc(proc))) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        ORTE_RELEASE_THREAD(&local_ctl);
        return NULL;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s ess:orcm: proc %s is on host %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(proc),
                         pdata->node->name));
    
    ORTE_RELEASE_THREAD(&local_ctl);
    return pdata->node->name;
}

static orte_local_rank_t proc_get_local_rank(orte_process_name_t *proc)
{
    orte_proc_t *pdata;
    
    ORTE_ACQUIRE_THREAD(&local_ctl);

    if (NULL == (pdata = find_proc(proc))) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        ORTE_RELEASE_THREAD(&local_ctl);
        return ORTE_LOCAL_RANK_INVALID;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s ess:orcm: proc %s has local rank %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(proc),
                         (int)pdata->local_rank));
    
    ORTE_RELEASE_THREAD(&local_ctl);
    return pdata->local_rank;
}

static orte_node_rank_t proc_get_node_rank(orte_process_name_t *proc)
{
    orte_proc_t *pdata;
    
    ORTE_ACQUIRE_THREAD(&local_ctl);

    if (NULL == (pdata = find_proc(proc))) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        ORTE_RELEASE_THREAD(&local_ctl);
        return ORTE_NODE_RANK_INVALID;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s ess:orcm: proc %s has node rank %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(proc),
                         (int)pdata->node_rank));
    
    ORTE_RELEASE_THREAD(&local_ctl);
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
    int32_t jfam;
    orte_job_t *jdata;
    int startup;

    /* get the time delay to allow wireup */
    mca_base_param_reg_int_name("orte", "wireup_timeout",
                                "Time in secs for scheduler to wait to hear from daemons at startup [default: 3]",
                                false, false, 3, &startup);
    timeout_tv.tv_sec = startup;

    /* see if this is a clean restart - i.e., all pre-existing jobs are
     * to be terminated
     */
    mca_base_param_reg_int_name("orcm", "sched_clean_start",
                                "Kill any pre-existing processes [default: 0]",
                                false, false, (int)false, &fd);
    clean_startup = OPAL_INT_TO_BOOL(fd);

    /* initialize the global list of local children and job data - although
     * we never have local children ourselves, the TCP multicast requires
     * that these lists at least exist
     */
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
    
    /* Setup the job data object for the daemons - remember, we are
     * one of them, taking the vpid=1 position!
     */        
    /* create and store the job data object */
    daemons = OBJ_NEW(orte_job_t);
    daemons->jobid = ORTE_PROC_MY_NAME->jobid;
    daemons->name = strdup("ORCM DVM");
    daemons->instance = strdup(ORTE_JOBID_PRINT(ORTE_PROC_MY_NAME->jobid));
    /* daemons always recover */
    daemons->recovery_defined = true;
    daemons->enable_recovery = true;
    /* mark the state as UNDEFINED as the daemons haven't announced
     * to us yet
     */
    daemons->state = ORTE_JOB_STATE_RUNNING;
    /* create an app */
    app = OBJ_NEW(orte_app_context_t);
    app->app = strdup("orcm-sched");
    app->name = strdup("orcm-sched");
    app->num_procs = 1;
    /* scheduler always has infinite restarts */
    app->recovery_defined = true;
    app->max_restarts = -1;
    opal_argv_append_nosize(&app->argv, "orcm-sched");
    /* add to the daemon job - always must be an app for a job */
    opal_pointer_array_add(daemons->apps, app);
    /* add another app for the daemons themselves */
    daemon_app = OBJ_NEW(orte_app_context_t);
    daemon_app->app = strdup("orcmd");
    daemon_app->name = strdup("orcmd");
    /* daemons always have infinite restarts */
    daemon_app->recovery_defined = true;
    daemon_app->max_restarts = -1;
    opal_argv_append_nosize(&daemon_app->argv, "orcmd");
    opal_pointer_array_add(daemons->apps, daemon_app);
    daemons->num_apps = 2;

    /* setup the daemon map so it knows how to map them */
    daemons->map = OBJ_NEW(orte_job_map_t);
    daemons->map->policy = ORTE_MAPPING_BYNODE;
    /* save it */
    opal_pointer_array_set_item(orte_job_data, 0, daemons);
   
    /* ensure our mapping policy will utilize any VM */
    ORTE_ADD_MAPPING_POLICY(ORTE_MAPPING_USE_VM);
    /* use bynode mapping by default */
    ORTE_ADD_MAPPING_POLICY(ORTE_MAPPING_BYNODE);

    /* We create and store a node object where we are
     * as this location just so we can report it nicely
     * when queried. However, since we do not locally spawn
     * our own child procs, we cannot consider this location
     * as a viable target until a daemon reports from it, so
     * we mark it as unavailable and let a daemon's report
     * overwrite the state
     */
    node = OBJ_NEW(orte_node_t);
    node->name = strdup(orte_process_info.nodename);
    node->state = ORTE_NODE_STATE_NOT_INCLUDED;
    node->slots = 0;  /* min number */
    node->slots_alloc = node->slots;
    node->index = ORTE_PROC_MY_NAME->vpid;
    opal_pointer_array_set_item(orte_node_pool, ORTE_PROC_MY_NAME->vpid, node);

    /* create and store a proc object for us */
    proc = OBJ_NEW(orte_proc_t);
    proc->name.jobid = ORTE_PROC_MY_NAME->jobid;
    proc->name.vpid = ORTE_PROC_MY_NAME->vpid;
    proc->pid = orte_process_info.pid;
    proc->app_idx = 0;
    proc->state = ORTE_PROC_STATE_RUNNING;
    OBJ_RETAIN(node);  /* keep accounting straight */
    proc->node = node;
    proc->nodename = node->name;
    opal_pointer_array_set_item(daemons->procs, proc->name.vpid, proc);
    daemons->num_procs = 1;

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
    
    /* open the plm base so we initialize that framework's
     * global values - we don't select a module as we
     * don't need one
     */
    if (ORTE_SUCCESS != (ret = orte_plm_base_open())) {
        ORTE_ERROR_LOG(ret);
        error = "orte_plm_base_open";
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
     * if it wasn't given to us
     */
    if (NULL == orte_process_info.my_daemon_uri) {
        orte_process_info.my_daemon_uri = orte_rml.get_contact_info();
        ORTE_PROC_MY_DAEMON->jobid = ORTE_PROC_MY_NAME->jobid;
        ORTE_PROC_MY_DAEMON->vpid = ORTE_PROC_MY_NAME->vpid;
    }
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
    
    /* be sure to update the routing tree so the initial "phone home"
     * to mpirun goes through the tree!
     */
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
        ORTE_ERROR_LOG(ret);
        orte_quit();
    }
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcm-stop", "0.1", "alpha",
                                                         ORCM_PNP_SYS_CHANNEL,
                                                         ORCM_PNP_TAG_TERMINATE,
                                                         vm_term, NULL))) {
        ORTE_ERROR_LOG(ret);
        orte_quit();
    }
    /* listen for PS requests */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcm-ps", "0.1", "alpha",
                                                         ORCM_PNP_SYS_CHANNEL,
                                                         ORCM_PNP_TAG_PS,
                                                         ps_request, NULL))) {
        ORTE_ERROR_LOG(ret);
        orte_quit();
    }
    /* listen for state data when restarting */
    if (ORCM_SUCCESS != (ret = orcm_pnp.register_receive("orcmd", "0.1", "alpha",
                                                         ORCM_PNP_SYS_CHANNEL,
                                                         ORCM_PNP_TAG_DATA,
                                                         process_contact, NULL))) {
        ORTE_ERROR_LOG(ret);
        orte_quit();
    }

    /* open the cfgi framework so we can receive configuration instructions */
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

    /* define an event to handle processing of daemon replies */
    if (pipe(process_pipe) < 0) {
        error = "cannot open event pipe";
        goto error;
    }
    opal_event_set(opal_event_base, &process_ev, process_pipe[0],
                   OPAL_EV_READ|OPAL_EV_PERSIST, process_daemon, NULL);
    opal_event_add(&process_ev, 0);

    /* flag that we are starting up */
    bootstrap_complete = false;

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

    /* we are cold booting, so we need to wait to
     * hear from other daemons in the system before
     * we start receiving configuration info and trying
     * to launch processes
     */
    opal_event_evtimer_set(opal_event_base, &timeout, release, &timeout);
    /* fake out the thread ctl by flagging it as active
     * so we will sit on a conditioned wait
     */
    ctl.active = true;
    /* announce our existence - this carries with it our rml uri and
     * our local node system info
     */
    if (ORCM_SUCCESS != (ret = orcm_pnp.announce("ORCM-SCHED", "0.1", "alpha", vm_tracker))) {
        ORTE_ERROR_LOG(ret);
        error = "announce";
        goto error;
    }
    /* start the timer */
    opal_event_evtimer_add(&timeout, &timeout_tv);
    /* wait to acquire the thread */
    ORTE_ACQUIRE_THREAD(&ctl);

    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s BOOTSTRAP COMPLETED",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));

    /* delete our local event */
    opal_event_del(&process_ev);
    close(process_pipe[0]);
    close(process_pipe[1]);

    /* let people know we are alive */
    opal_output(orte_clean_output, "ORCM SCHEDULER %s IS READY",
                ORTE_JOB_FAMILY_PRINT(ORTE_PROC_MY_NAME->jobid));

    /* start the local sensors - do this last so heartbeats don't
     * start running too early
     */
    orte_sensor.start(ORTE_PROC_MY_NAME->jobid);

    /* enable configuration */
    orcm_cfgi_base_activate();

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
    orte_plm_base_close();
    orte_ras_base_close();
    orte_rmaps_base_close();
    orte_errmgr_base_close();

    /* close the orcm-related frameworks */
    orcm_leader_base_close();
    orcm_cfgi_base_close();
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

static void vm_tracker(orcm_info_t *vm)
{
    orte_proc_t *proc;
    orte_node_t *node;
    int i, rc;
    uint8_t trig=0;
    opal_buffer_t *buf;
    orte_daemon_cmd_flag_t command;
    bool restarted;

    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s Received announcement from %s:%s:%s proc %s on node %s pid %lu",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), vm->app, vm->version, vm->release,
                         ORTE_NAME_PRINT(vm->name), vm->nodename, (unsigned long)vm->pid));
    
    ORTE_ACQUIRE_THREAD(&local_ctl);

    /* if this isn't one of my peers, ignore it */
    if (vm->name->jobid != ORTE_PROC_MY_NAME->jobid) {
        /* if this is an orcm or orcmd that belongs to this user, then we have a problem */
        if ((0 == strcasecmp(vm->app, "orcmd") || (0 == strcasecmp(vm->app, "orcm"))) && vm->uid == my_uid) {
            orte_show_help("help-orcm.txt", "preexisting-orcmd", true, vm->nodename);
        }
        ORTE_RELEASE_THREAD(&local_ctl);
        return;
    }

    /* if this is vpid=0, we ignore it - it is the "orcm" launch tool and, while part
     * of the daemon job, is not capable of launching or monitoring processes
     */
    if (0 == vm->name->vpid) {
        goto release;
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
        proc->app_idx = 1;  /* point to the orcmd app_context */
        daemon_app->num_procs++;
        proc->rml_uri = strdup(vm->rml_uri);
        daemons->num_procs++;
        opal_pointer_array_set_item(daemons->procs, vm->name->vpid, proc);
        /* setup to complete the handshake */
        buf = OBJ_NEW(opal_buffer_t);
        /* identify the desired respondent */
        opal_dss.pack(buf, vm->name, 1, ORTE_NAME);
        /* if this is a clean start, tell the daemon to kill any
         * existing running procs
         */
        if (clean_startup) {
            command = ORTE_DAEMON_KILL_LOCAL_PROCS;
        } else {
            /* ask for an update of current state */
            command = ORTE_DAEMON_CHECKIN_CMD;
        }
        opal_dss.pack(buf, &command, 1, ORTE_DAEMON_CMD_T);
        /* send it*/
        if (ORTE_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_SYS_CHANNEL, NULL,
                                                     ORCM_PNP_TAG_DATA, NULL, 0,
                                                     buf, cbfunc, NULL))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(buf);
        }
        /* flag that it wasn't restarted */
        restarted = false;
    } else {
        /* this daemon must have restarted */
        OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                             "%s DAEMON %s HAS RESTARTED",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(vm->name)));
        daemons->num_procs++;
        /* re-initialize heartbeat to avoid race condition with
         * heartbeat check timer
         */
        proc->beat = 0;
        restarted = true;
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
    /* initialize heartbeat */
    proc->beat = 0;

    /* get the node - it is at the index of the daemon's vpid */
    if (NULL != (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, vm->name->vpid))) {
        /* already have this node - could be a race condition
         * where the daemon died and has been replaced, so
         * just assume that is the case
         */
        OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                             "%s EXISTING NODE %s(%s) IS BEING SET TO UP",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             node->name, ORTE_NAME_PRINT(vm->name)));
        if (NULL != node->daemon) {
            OBJ_RELEASE(node->daemon);
        }
        node->state = ORTE_NODE_STATE_UP;
        goto complete;
    }
    OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                         "%s ADDING NODE %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), vm->nodename));
    /* if we get here, this is a new node */
    node = OBJ_NEW(orte_node_t);
    node->name = strdup(vm->nodename);
    node->state = ORTE_NODE_STATE_UP;
    node->slots = 1;  /* min number */
    node->slots_alloc = node->slots;
    node->index = vm->name->vpid;
    opal_pointer_array_set_item(orte_node_pool, vm->name->vpid, node);
 complete:
    OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                         "%s WIRING NODE %s TO DAEMON %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         node->name, ORTE_NAME_PRINT(&proc->name)));
    OBJ_RETAIN(node);  /* maintain accounting */
    proc->node = node;
    proc->nodename = node->name;
    OBJ_RETAIN(proc);  /* maintain accounting */
    node->daemon = proc;
    node->daemon_launched = true;

    /* if this daemon restarted, then check jobs for procs
     * waiting to migrate
     */
    if (restarted) {
        opal_output(0, "%s RESTART OF PROCS AWAITING RESOURCES IS NOT ENABLED YET",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
#if 0
        orte_errmgr.update_state(ORTE_JOBID_WILDCARD, ORTE_JOB_STATE_PROCS_MIGRATING,
                                 NULL, ORTE_PROC_STATE_MIGRATING, 0, 0);
#endif
    }

 release:
    /* reset the timer */
    if (!bootstrap_complete) {
        OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                             "%s RESETTING TIMER FROM ANNOUNCEMENT",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        opal_fd_write(process_pipe[1], sizeof(uint8_t), &trig);
    }
    ORTE_RELEASE_THREAD(&local_ctl);
    return;
}

static void process_daemon(int fd, short flag, void *dump)
{
    uint8_t trig;

    /* clear the trigger pipe */
    opal_fd_read(process_pipe[0], sizeof(uint8_t), &trig);
    opal_event_evtimer_add(&timeout, &timeout_tv);
}

static void release(int fd, short flag, void *dump)
{
    int i;
    orte_proc_t *proc;

    ORTE_ACQUIRE_THREAD(&local_ctl);
    /* flag that we are done with our own startup */
    bootstrap_complete = true;
    OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                         "%s TIMER COMPLETE - RELEASING BOOTSTRAP",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    /* report any missing responses */
    for (i=2; i < daemons->procs->size; i++) {
        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(daemons->procs, i))) {
            continue;
        }
        if (!proc->reported) {
            opal_output(0, "%s MISSING REPORT FROM DAEMON %s on NODE %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&proc->name),
                        proc->node->name);
        }
    }
    ORTE_RELEASE_THREAD(&local_ctl);
    ORTE_WAKEUP_THREAD(&ctl);
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
    opal_buffer_t *ans;
    orte_vpid_t vpid=ORTE_VPID_INVALID;
    orte_app_idx_t idx;
    int i, j, k;
    orte_job_t *jdata;
    orte_app_context_t *app;
    orte_proc_t *proc;
    char *null="[**]";
    char nodename[64], *nn;

    ORTE_ACQUIRE_THREAD(&local_ctl);

    /* unpack the target name */
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &name, &n, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        ORTE_RELEASE_THREAD(&local_ctl);
        return;
    }
    
    /* if the requested job family isn't mine, then ignore it */
    if (ORTE_JOB_FAMILY(name.jobid) != ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid)) {
        ORTE_RELEASE_THREAD(&local_ctl);
        return;
    }
    
    /* construct the response */
    ans = OBJ_NEW(opal_buffer_t);
    
    /* cycle thru the running jobs */
    for (i=0; i < orte_job_data->size; i++) {
        if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
            continue;
        }
        opal_dss.pack(ans, &jdata->jobid, 1, ORTE_JOBID);
        opal_dss.pack(ans, &jdata->name, 1, OPAL_STRING);
        /* cycle thru the apps in this job */
        for (k=0; k < jdata->apps->size; k++) {
            if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, k))) {
                continue;
            }
            /* record the app data */
            idx = k;
            opal_dss.pack(ans, &idx, 1, ORTE_APP_IDX);
            opal_dss.pack(ans, &app->app, 1, OPAL_STRING);
            opal_dss.pack(ans, &app->max_restarts, 1, OPAL_INT32);
            /* cycle thru the procs in this job */
            for (j=0; j < jdata->procs->size; j++) {
                if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, j))) {
                    continue;
                }
                if (proc->app_idx != k) {
                    continue;
                }
                /* record this proc's data */
                opal_dss.pack(ans, &proc->name.vpid, 1, ORTE_VPID);
                opal_dss.pack(ans, &proc->pid, 1, OPAL_PID);
                if (NULL == proc->node || NULL == proc->node->name) {
                    opal_dss.pack(ans, &null, 1, OPAL_STRING);
                } else {
                    if (NULL == proc->node->daemon) {
                        snprintf(nodename, 64, "%s[--]", proc->node->name);
                    } else {
                        snprintf(nodename, 64, "%s[%s]", proc->node->name, ORTE_VPID_PRINT(proc->node->daemon->name.vpid));
                    }
                    nn = nodename;
                    opal_dss.pack(ans, &nn, 1, OPAL_STRING);
                }
                opal_dss.pack(ans, &proc->restarts, 1, OPAL_INT32);
            }
            /* write an end-of-data marker */
            opal_dss.pack(ans, &vpid, 1, ORTE_VPID);
        }
        /* write an end-of-data marker */
        idx = ORTE_APP_IDX_MAX;
        opal_dss.pack(ans, &idx, 1, ORTE_APP_IDX);
    }

    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_SYS_CHANNEL,
                                                 NULL, ORCM_PNP_TAG_PS,
                                                 NULL, 0, ans, cbfunc, NULL))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(ans);
    }
    ORTE_RELEASE_THREAD(&local_ctl);
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

    OPAL_OUTPUT_VERBOSE((1, orte_ess_base_output,
                         "%s GOT TERM COMMAND FROM %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));

    ORTE_ACQUIRE_THREAD(&local_ctl);

    /* if this isn't intended for me, ignore it */
    n=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &jfam, &n, OPAL_UINT16))) {
        ORTE_ERROR_LOG(rc);
        ORTE_RELEASE_THREAD(&local_ctl);
        return;
    }
    if (jfam != ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid)) {
        OPAL_OUTPUT_VERBOSE((1, orte_ess_base_output,
                             "%s GOT TERM COMMAND FOR DVM %d - NOT FOR ME!",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), jfam));
        ORTE_RELEASE_THREAD(&local_ctl);
        return;
    }

    ORTE_RELEASE_THREAD(&local_ctl);
    ORTE_TIMER_EVENT(0, 0, orcm_just_quit);
}

static void process_contact(int status,
                            orte_process_name_t *peer,
                            orcm_pnp_tag_t tag,
                            struct iovec *msg, int count,
                            opal_buffer_t *buffer,
                            void *cbdata)
{
    int n, rc, ret;
    int32_t num, i;
    orte_job_t *jdata;
    orte_app_context_t *app;
    char *instance, *name;
    orte_process_name_t child;
    orte_proc_t *proc;
    bool newjob;
    orte_app_idx_t num_apps, na, app_idx;
    orte_node_t *node, *nd;
    bool nodepresent;
    orte_jobid_t jobid;
    pid_t pid;
    orte_proc_state_t state;
    int32_t incarnation;
    uint8_t trig=1;

    ORTE_ACQUIRE_THREAD(&local_ctl);

    /* get the response status */
    n = 1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &ret, &n, OPAL_INT))) {
        ORTE_ERROR_LOG(rc);
        goto release;
    }

    if (ORTE_SUCCESS != ret) {
        opal_output(0, "%s DAEMON %s RETURNED ERROR %s ON CONTACT",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    ORTE_NAME_PRINT(peer),
                    ORTE_ERROR_NAME(ret));
        goto release;
    }

    /* process any returned state info */
    if (!clean_startup) {
        OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                             "%s PROCESSING STATE INFO FROM %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(peer)));
        /* find the node object for this daemon */
        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(daemons->procs, peer->vpid))) {
            /* unknown daemon - should never happen! */
            opal_output(0, "%s UNKNOWN DAEMON RESPONDED TO CONTACT REQUEST",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_NAME_PRINT(peer));
            goto release;
        }
        /* flag that it reported */
        proc->reported = true;
        if (NULL == (node = proc->node)) {
            opal_output(0, "%s DAEMON %s ON UNKNOWN NODE",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_NAME_PRINT(peer));
            goto release;
        }
        /* get the number of job data entries coming back */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &num, &n, OPAL_INT32))) {
            ORTE_ERROR_LOG(rc);
            goto release;
        }
        OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                             "%s RECVD %d JOB DATA ENTRIES FROM %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), num,
                             ORTE_NAME_PRINT(peer)));
        for (i=0; i < num; i++) {
            /* get jobid */
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &jobid, &n, ORTE_JOBID))) {
                ORTE_ERROR_LOG(rc);
                goto release;
            }
            /* see if we already have this job */
            newjob = false;
            if (NULL == (jdata = orte_get_job_data_object(jobid))) {
                jdata = OBJ_NEW(orte_job_t);
                jdata->jobid = jobid;
                opal_pointer_array_set_item(orte_job_data, ORTE_LOCAL_JOBID(jobid), jdata);
                /* ensure the next jobid is reset to avoid collision */
                if (orte_plm_globals.next_jobid <= ORTE_LOCAL_JOBID(jobid)) {
                    orte_plm_globals.next_jobid = ORTE_LOCAL_JOBID(jobid) + 1;
                }
                newjob = true;
                OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                                     "%s PROCESSING DATA FOR NEW JOB %s FROM %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_JOBID_PRINT(jobid),
                                     ORTE_NAME_PRINT(peer)));
            } else {
                OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                                     "%s IGNORING DATA FOR JOB %s FROM %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_JOBID_PRINT(jobid),
                                     ORTE_NAME_PRINT(peer)));
            }
            /* get instance */
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &instance, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                goto release;
            }
            if (newjob) {
                jdata->instance = instance;
            } else {
                free(instance);
            }
            /* get name*/
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &name, &n, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                goto release;
            }
            if (newjob) {
                jdata->name = name;
            } else {
                free(name);
            }
            /* get number of app_contexts */
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &num_apps, &n, ORTE_APP_IDX))) {
                ORTE_ERROR_LOG(rc);
                goto release;
            }
            jdata->num_apps = num_apps;
            /* get each app_context - we do it this way instead of packing
             * them as a group so we don't have to allocate space as many
             * of these entries will be duplicates of things we already
             * recvd from other daemons
             */
            for (na=0; na < num_apps; na++) {
                n=1;
                if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &app, &n, ORTE_APP_CONTEXT))) {
                    ORTE_ERROR_LOG(rc);
                    goto release;
                }
                if (!newjob) {
                    OBJ_RELEASE(app);
                    continue;
                }
                /* add it to the job object */
                opal_pointer_array_set_item(jdata->apps, na, app);
            }
        }
        /* get the number of children on this daemon */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &num, &n, OPAL_INT32))) {
            ORTE_ERROR_LOG(rc);
            goto release;
        }
        if (0 == num) {
            /* nobody there */
            OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                                 "%s NO LOCAL CHILDREN ON %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(peer)));
            goto release;
        }
        OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                             "%s GOT %d LOCAL CHILDREN ON %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), num,
                             ORTE_NAME_PRINT(peer)));
        for (i=0; i < num; i++) {
            /* get name */
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &child, &n, ORTE_NAME))) {
                ORTE_ERROR_LOG(rc);
                goto release;
            }
            /* get the job object for this child */
            if (NULL == (jdata = orte_get_job_data_object(child.jobid))) {
                ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
                goto release;
            }
            /* get app index */
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &app_idx, &n, ORTE_APP_IDX))) {
                ORTE_ERROR_LOG(rc);
                goto release;
            }
            /* get pid */
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &pid, &n, OPAL_PID))) {
                ORTE_ERROR_LOG(rc);
                goto release;
            }
            /* get state */
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &state, &n, ORTE_PROC_STATE))) {
                ORTE_ERROR_LOG(rc);
                goto release;
            }
            /* get incarnation */
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &incarnation, &n, OPAL_INT32))) {
                ORTE_ERROR_LOG(rc);
                goto release;
            }
            /* find child in this job */
            if (NULL != (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, child.vpid))) {
                opal_output(0, "%s CHILD %s ALREADY KNOWN - SHOULD NOT HAPPEN",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_NAME_PRINT(&child));
                goto release;
            }
            proc = OBJ_NEW(orte_proc_t);
            proc->name.jobid = child.jobid;
            proc->name.vpid = child.vpid;
            proc->app_idx = app_idx;
            proc->pid = pid;
            proc->state = state;
            proc->restarts = incarnation;
            /* connect it to this node */
            OBJ_RETAIN(node);
            proc->node = node;
            OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                                 "%s ADDING CHILD %s TO %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(&child),
                                 ORTE_JOBID_PRINT(jdata->jobid)));
            /* add it to the job */
            opal_pointer_array_set_item(jdata->procs, child.vpid, proc);
            jdata->num_procs++;
            /* add it to the node */
            OBJ_RETAIN(proc);
            opal_pointer_array_add(node->procs, proc);
            node->num_procs++;
            /* ensure we have a map, and that the node is there */
            if (NULL == jdata->map) {
                jdata->map = OBJ_NEW(orte_job_map_t);
            }
            nodepresent = false;
            for (n=0; n < jdata->map->nodes->size; n++) {
                if (NULL == (nd = (orte_node_t*)opal_pointer_array_get_item(jdata->map->nodes, n))) {
                    continue;
                }
                if (nd->index == peer->vpid) {
                    /* node already present */
                    nodepresent = true;
                    break;
                }
            }
            if (!nodepresent) {
                OPAL_OUTPUT_VERBOSE((5, orte_ess_base_output,
                                     "%s ADDING NODE %s TO MAP FOR JOB %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     node->name, ORTE_JOBID_PRINT(jdata->jobid)));
                OBJ_RETAIN(node);
                opal_pointer_array_add(jdata->map->nodes, node);
                jdata->map->num_nodes++;
            }
        }
    }

 release:
    if (!bootstrap_complete) {
        OPAL_OUTPUT_VERBOSE((2, orte_ess_base_output,
                             "%s RESETTING TIMER FROM CONTACT REPLY",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        opal_fd_write(process_pipe[1], sizeof(uint8_t), &trig);
    }
    ORTE_RELEASE_THREAD(&local_ctl);
}

/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 *
 * The ORCM daemon (orcmd) operates in an independent manner. Each orcmd
 * is responsible only for launching and monitoring its own local procs.
 * State communication between orcmd's, therefore, is conducted only for
 * the purpose of migrating processes away from a given node and for
 * notifying other procs of the potential need to change communication
 * "leader".
 *
 * Thus, the orcmd errmgr module is tasked with:
 *
 * (a) given failure of a local proc, attempt to locally restart
 *     it up to the max local restarts
 *
 * (b) once a proc exceeds its max local restarts, send a message to
 *     all other daemons indicating that this proc requires "global"
 *     restart. Each of the receiving daemons will compute the location
 *     that should attempt this restart - since each daemon will use the
 *     same algorithm, they should reach the same conclusion. The daemon
 *     that identifies itself as the new host is responsible for
 *     launching the replacement proc
 *
 * (c) in both of the above cases, the errmgr module will notify the
 *     orcm errmgr framework of all orcmd's that this proc
 *     failed, and the orcmds will pass that to their local procs.
 *     This will allow the application procs to determine if
 *     the respective communication "leader" needs to be updated.
 *
 * Note some of the things we don't do that are otherwise done in the
 * ORTE components:
 *
 * (a) send proc state updates. There is no HNP in orcm, so no need
 *     for everyone to know the state. All any daemon needs to know
 *     is what procs are where for potential routing purposes. so we
 *     only notify of failures and terminations.
 */

#include "openrcm_config_private.h"
#include "include/constants.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "opal/util/output.h"
#include "opal/util/opal_sos.h"
#include "opal/dss/dss.h"

#include "orte/util/error_strings.h"
#include "orte/util/name_fns.h"
#include "orte/util/proc_info.h"
#include "orte/util/session_dir.h"
#include "orte/util/show_help.h"
#include "orte/runtime/orte_globals.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/plm/plm_types.h"
#include "orte/mca/rmaps/rmaps.h"
#include "orte/mca/routed/routed.h"
#include "orte/mca/sensor/sensor.h"
#include "orte/orted/orted.h"

#include "mca/pnp/pnp.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/errmgr/base/base.h"
#include "orte/mca/errmgr/base/errmgr_private.h"

#include "errmgr_orcmd.h"

/* Local functions */
static bool any_live_children(orte_jobid_t job);
static int pack_state_update(opal_buffer_t *alert, orte_odls_job_t *jobdat);
static int pack_state_for_proc(opal_buffer_t *alert, orte_odls_child_t *child);
static bool all_children_registered(orte_jobid_t job);
static int pack_child_contact_info(orte_jobid_t job, opal_buffer_t *buf);
static void failed_start(orte_odls_job_t *jobdat, orte_exit_code_t exit_code);
static void update_local_children(orte_odls_job_t *jobdat,
                                  orte_job_state_t jobstate,
                                  orte_proc_state_t state);
static void killprocs(orte_jobid_t job, orte_vpid_t vpid);
static void callback(int status,
                     orte_process_name_t *sender,
                     orcm_pnp_tag_t tag,
                     struct iovec *msg,
                     int count,
                     opal_buffer_t *buf,
                     void *cbdata);
static void remote_update(int status,
                          orte_process_name_t *sender,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg,
                          int count,
                          opal_buffer_t *buf,
                          void *cbdata);

/*
 * Module functions: Global
 */
static int init(void);
static int finalize(void);

static int predicted_fault(opal_list_t *proc_list,
                           opal_list_t *node_list,
                           opal_list_t *suggested_nodes);

static int update_state(orte_jobid_t job,
                        orte_job_state_t jobstate,
                        orte_process_name_t *proc,
                        orte_proc_state_t state,
                        pid_t pid,
                        orte_exit_code_t exit_code);

static int suggest_map_targets(orte_proc_t *proc,
                               orte_node_t *oldnode,
                               opal_list_t *node_list);

static int ft_event(int state);



/******************
 * ORCMD module
 ******************/
orte_errmgr_base_module_t orte_errmgr_orcmd_module = {
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
static opal_mutex_t lock;
static opal_condition_t cond;
static bool active;
static orte_job_t *daemon_job;
static void recover_procs(orte_process_name_t *daemon_that_failed);
static void notify_failure(orte_odls_job_t *jobdat, orte_odls_child_t *child, bool recover);

/************************
 * API Definitions
 ************************/
static int init(void)
{
    int rc;

    /* construct the globals */
    OBJ_CONSTRUCT(&lock, opal_mutex_t);
    OBJ_CONSTRUCT(&cond, opal_condition_t);
    active = false;

    /* get the daemon job object */
    if (NULL == (daemon_job = orte_get_job_data_object(ORTE_PROC_MY_NAME->jobid))) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        return ORTE_ERR_NOT_FOUND;
    }

    /* setup to recv errmgr updates from our peers */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_receive("orcmd", "0.1", "alpha",
                                                        ORCM_PNP_SYS_CHANNEL,
                                                        ORCM_PNP_TAG_ERRMGR,
                                                        remote_update, NULL))) {
        ORTE_ERROR_LOG(rc);
    }
    return rc;
}

static int finalize(void)
{
    OBJ_DESTRUCT(&lock);
    OBJ_DESTRUCT(&cond);

    orcm_pnp.cancel_receive("orcmd", "0.1", "alpha", ORCM_PNP_SYS_CHANNEL, ORCM_PNP_TAG_ERRMGR);
    return ORTE_SUCCESS;
}

static int update_state(orte_jobid_t job,
                        orte_job_state_t jobstate,
                        orte_process_name_t *proc,
                        orte_proc_state_t state,
                        pid_t pid,
                        orte_exit_code_t exit_code)
{
    opal_list_item_t *item, *next;
    orte_odls_job_t *jobdat;
    orte_odls_child_t *child;
    int rc=ORTE_SUCCESS;
    orte_vpid_t null=ORTE_VPID_INVALID;
    orte_app_context_t *app;
    
    OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                         "%s errmgr:update_state for job %s proc %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_JOBID_PRINT(job),
                         (NULL == proc) ? "NULL" : ORTE_NAME_PRINT(proc)));

    /* protect against threads */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);

    /*
     * if orte is trying to shutdown, just let it
     */
    if (orte_finalizing) {
        opal_output(0, "FINALIZING");
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORTE_SUCCESS;
    }
    
    /***   UPDATE COMMAND FOR A JOB   ***/
    if (NULL == proc) {
        /* this is an update for an entire job */
        if (ORTE_JOBID_INVALID == job) {
            /* whatever happened, we don't know what job
             * it happened to
             */
            orte_show_help("help-orte-errmgr-orcmd.txt", "errmgr-orcmd:unknown-job-error",
                           true, orte_job_state_to_str(jobstate));
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            return ORTE_ERROR;
        }

        /* lookup the local jobdat for this job */
        jobdat = NULL;
        for (item = opal_list_get_first(&orte_local_jobdata);
             item != opal_list_get_end(&orte_local_jobdata);
             item = opal_list_get_next(item)) {
            jobdat = (orte_odls_job_t*)item;

            /* is this the specified job? */
            if (jobdat->jobid == job) {
                break;
            }
        }
        if (NULL == jobdat) {
            OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                                 "%s JOBDAT FOR JOB %s NOT FOUND",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_JOBID_PRINT(job)));
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            return ORTE_ERR_NOT_FOUND;
        }
        
        switch (jobstate) {
            /* first handle the cases where no notifications
             * are sent to our peers
             */
        case ORTE_JOB_STATE_RUNNING:
            /* update all local child states - there is no need
             * to send this state elsewhere
             */
            update_local_children(jobdat, jobstate, ORTE_PROC_STATE_RUNNING);
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            return ORTE_SUCCESS;
        case ORTE_JOB_STATE_COMM_FAILED:
            /* kill all local procs */
            killprocs(ORTE_JOBID_WILDCARD, ORTE_VPID_WILDCARD);
            /* tell the caller we can't recover */
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            return ORTE_ERR_UNRECOVERABLE;
        case ORTE_JOB_STATE_HEARTBEAT_FAILED:
            /* should never happen */
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            return ORTE_SUCCESS;

            /* now handle the cases where notification is required */
        case ORTE_JOB_STATE_FAILED_TO_START:
            /* mark all local procs for this job as failed to start */
            failed_start(jobdat, exit_code);
            /* send a notification */
            notify_failure(jobdat, NULL, NULL);
            break;
        case ORTE_JOB_STATE_SENSOR_BOUND_EXCEEDED:
            /* update all procs in job */
            update_local_children(jobdat, jobstate, ORTE_PROC_STATE_SENSOR_BOUND_EXCEEDED);
            /* order all local procs for this job to be killed */
            killprocs(jobdat->jobid, ORTE_VPID_WILDCARD);
            /* send a notification */
            notify_failure(jobdat, NULL, NULL);
            break;

        default:
            break;
        }
        /* release thread */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return rc;
    }

    /* if this was a failed comm or heartbeat */
    if (ORTE_PROC_STATE_COMM_FAILED == state ||
        ORTE_PROC_STATE_HEARTBEAT_FAILED == state) {
        /* if this isn't a daemon proc, ignore it */
        if (ORTE_PROC_MY_NAME->jobid != proc->jobid) {
            OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                                 "%s Received %s for proc %s - not a daemon",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 orte_proc_state_to_str(state),
                                 ORTE_NAME_PRINT(proc)));
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            return ORTE_SUCCESS;
        }
        /* if it is our own connection, ignore it - this can
         * happen due to a race condition when we shutdown
         */
        if (ORTE_PROC_MY_NAME->vpid == proc->vpid) {
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            return ORTE_SUCCESS;
        }
        /* delete the route */
        orte_routed.delete_route(proc);
        /* purge the oob */
        orte_rml.purge(proc);
        /* see if this was a lifeline - only applicable when we
         * are running in developer's mode so that we can cleanly
         * terminate the DVM with a ctrl-c
         */
        if (ORTE_SUCCESS != orte_routed.route_lost(proc)) {
            /* kill our children */
            killprocs(ORTE_JOBID_WILDCARD, ORTE_VPID_WILDCARD);
            /* tell the caller we can't recover */
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            return ORTE_ERR_UNRECOVERABLE;
        }
        /* if not, then we need to recover the procs
         * that were being hosted by that daemon
         */
        recover_procs(proc);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORTE_SUCCESS;
    }
    
    /* lookup the local jobdat for this job */
    jobdat = NULL;
    for (item = opal_list_get_first(&orte_local_jobdata);
         item != opal_list_get_end(&orte_local_jobdata);
         item = opal_list_get_next(item)) {
        jobdat = (orte_odls_job_t*)item;
        
        /* is this the specified job? */
        if (jobdat->jobid == proc->jobid) {
            break;
        }
    }
    if (NULL == jobdat) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORTE_ERR_NOT_FOUND;
    }
    
    /* find this proc in the local children */
    child = NULL;
    for (item = opal_list_get_first(&orte_local_children);
         item != opal_list_get_end(&orte_local_children);
         item = opal_list_get_next(item)) {
        child = (orte_odls_child_t*)item;
        if (child->name->jobid == proc->jobid &&
            child->name->vpid == proc->vpid) {
            break;
        }
    }
    if (NULL == child) {
        opal_output(0, "%s Child %s not found!",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    ORTE_NAME_PRINT(proc));
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORTE_ERR_NOT_FOUND;
    }

    OPAL_OUTPUT_VERBOSE((5, orte_errmgr_base.output,
                         "%s errmgr:orcmd got state %s for proc %s pid %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         orte_proc_state_to_str(state),
                         ORTE_NAME_PRINT(proc), pid));
 
    /***  UPDATE COMMAND FOR A SPECIFIC PROCESS ***/
    if (ORTE_PROC_STATE_SENSOR_BOUND_EXCEEDED == state) {
        if (ORTE_PROC_STATE_UNTERMINATED > child->state) {
            opal_output(0, "%s Process %s on node %s terminated: sensor bound exceeded",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(proc), orte_process_info.nodename);
            child->state = state;
            child->exit_code = exit_code;
            /* Decrement the number of local procs */
            jobdat->num_local_procs--;
            /* kill this proc */
            killprocs(proc->jobid, proc->vpid);
        }
        app = jobdat->apps[child->app_idx];
        if (jobdat->enable_recovery && child->restarts < app->max_local_restarts) {
            child->restarts++;
            OPAL_OUTPUT_VERBOSE((5, orte_errmgr_base.output,
                                 "%s errmgr:orcmd restarting proc %s for the %d time",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(proc), child->restarts));
            rc = orte_odls.restart_proc(child);
        }
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return rc;
    }
    
    if (ORTE_PROC_STATE_TERMINATED < state) {
        opal_output(0, "%s Process %s on node %s terminated: %s",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_NAME_PRINT(proc),
                    orte_process_info.nodename, orte_proc_state_to_str(state));
        /* see if we should recover it locally */
        if (jobdat->enable_recovery) {
            OPAL_OUTPUT_VERBOSE((5, orte_errmgr_base.output,
                                 "%s RECOVERY ENABLED",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
            /* see if this child has reached its local restart limit */
            app = jobdat->apps[child->app_idx];
            OPAL_OUTPUT_VERBOSE((5, orte_errmgr_base.output,
                                 "%s CHECKING RESTARTS %d VS MAX %d",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 child->restarts, app->max_local_restarts));
            if (child->restarts < app->max_local_restarts ) {
                /* notify our peers that this proc abnormally terminated
                 * and we are attempting local restart
                 */
                notify_failure(jobdat, child, true);
                /*  attempt to restart it locally */
                child->restarts++;
                OPAL_OUTPUT_VERBOSE((5, orte_errmgr_base.output,
                                     "%s errmgr:orcmd restarting proc %s for the %d time",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_NAME_PRINT(child->name), child->restarts));
                if (ORTE_SUCCESS != (rc = orte_odls.restart_proc(child))) {
                    /* reset the child's state as restart_proc would
                     * have cleared it
                     */
                    child->state = state;
                    ORTE_ERROR_LOG(rc);
                    goto LOCAL_ABORT;
                }
                OPAL_RELEASE_THREAD(&lock, &cond, &active);
                return ORTE_SUCCESS;
            }
        }

    LOCAL_ABORT:
        /* remove the child from our local list as it is no longer alive */
        opal_list_remove_item(&orte_local_children, &child->super);
        /* Decrement the number of local procs */
        jobdat->num_local_procs--;
        /* notify our peers that this proc abnormally terminated
         * and we are not attempting local restart
         */
        notify_failure(jobdat, child, false);
        /* release the child object */
        OBJ_RELEASE(child);

        /* cleanup */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORTE_SUCCESS;
    }    
    
    /* update proc state */
    if (ORTE_PROC_STATE_UNTERMINATED > child->state) {
        child->state = state;
        if (0 < pid) {
            child->pid = pid;
        }
        child->exit_code = exit_code;
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORTE_SUCCESS;
    }

    /* only other state is terminated - see if anyone is left alive */
    OPAL_OUTPUT_VERBOSE((5, orte_errmgr_base.output,
                         "%s errmgr:orcmd all procs in %s terminated",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_JOBID_PRINT(jobdat->jobid)));
        
    /* remove all of this job's children from the global list - do not lock
     * the thread as we are already locked
     */
    for (item = opal_list_get_first(&orte_local_children);
         item != opal_list_get_end(&orte_local_children);
         item = next) {
        child = (orte_odls_child_t*)item;
        next = opal_list_get_next(item);
            
        if (jobdat->jobid == child->name->jobid) {
            opal_list_remove_item(&orte_local_children, &child->super);
            OBJ_RELEASE(child);
        }
    }

    /* ensure the job's local session directory tree is removed */
    orte_session_dir_cleanup(jobdat->jobid);
        
    /* remove this job from our local job data since it is complete */
    opal_list_remove_item(&orte_local_jobdata, &jobdat->super);
    OBJ_RELEASE(jobdat);
        
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
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
static bool any_live_children(orte_jobid_t job)
{
    opal_list_item_t *item;
    orte_odls_child_t *child;
    
    /* the thread is locked elsewhere - don't try to do it again here */
    
    for (item = opal_list_get_first(&orte_local_children);
         item != opal_list_get_end(&orte_local_children);
         item = opal_list_get_next(item)) {
        child = (orte_odls_child_t*)item;
        
        /* is this child part of the specified job? */
        if ((job == child->name->jobid || ORTE_JOBID_WILDCARD == job) &&
            child->alive) {
            return true;
        }
    }

    /* if we get here, then nobody is left alive from that job */
    return false;
    
}

static int pack_state_for_proc(opal_buffer_t *alert, orte_odls_child_t *child)
{
    int rc;
    
    /* pack the child's vpid */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(alert, &(child->name->vpid), 1, ORTE_VPID))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    /* pack the pid */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(alert, &child->pid, 1, OPAL_PID))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    /* pack its state */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(alert, &child->state, 1, ORTE_PROC_STATE))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    /* pack its exit code */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(alert, &child->exit_code, 1, ORTE_EXIT_CODE))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    return ORTE_SUCCESS;
}

static int pack_state_update(opal_buffer_t *alert, orte_odls_job_t *jobdat)
{
    int rc;
    opal_list_item_t *item, *next;
    orte_odls_child_t *child;
    orte_vpid_t null=ORTE_VPID_INVALID;
    
    /* pack the jobid */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(alert, &jobdat->jobid, 1, ORTE_JOBID))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    for (item = opal_list_get_first(&orte_local_children);
         item != opal_list_get_end(&orte_local_children);
         item = next) {
        child = (orte_odls_child_t*)item;
        next = opal_list_get_next(item);
        /* if this child is part of the job... */
        if (child->name->jobid == jobdat->jobid) {
            if (ORTE_SUCCESS != (rc = pack_state_for_proc(alert, child))) {
                ORTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    /* flag that this job is complete so the receiver can know */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(alert, &null, 1, ORTE_VPID))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    return ORTE_SUCCESS;
}

static bool all_children_registered(orte_jobid_t job)
{
    opal_list_item_t *item;
    orte_odls_child_t *child;
    
    /* the thread is locked elsewhere - don't try to do it again here */
    
    for (item = opal_list_get_first(&orte_local_children);
         item != opal_list_get_end(&orte_local_children);
         item = opal_list_get_next(item)) {
        child = (orte_odls_child_t*)item;
        
        /* is this child part of the specified job? */
        if (OPAL_EQUAL == opal_dss.compare(&child->name->jobid, &job, ORTE_JOBID)) {
            /* if this child has terminated, we consider it as having
             * registered for the purposes of this function. If it never
             * did register, then we will send a NULL rml_uri back to
             * the HNP, which will then know that the proc did not register.
             * If other procs did register, then the HNP can declare an
             * abnormal termination
             */
            if (ORTE_PROC_STATE_UNTERMINATED < child->state) {
                /* this proc has terminated somehow - consider it
                 * as registered for now
                 */
                continue;
            }
            /* if this child is *not* registered yet, return false */
            if (!child->init_recvd) {
                return false;
            }
            /* if this child has registered a finalize, return false */
            if (child->fini_recvd) {
                return false;
            }
        }
    }
    
    /* if we get here, then everyone in the job is currently registered */
    return true;
    
}

static int pack_child_contact_info(orte_jobid_t job, opal_buffer_t *buf)
{
    opal_list_item_t *item;
    orte_odls_child_t *child;
    int rc;
    
    /* the thread is locked elsewhere - don't try to do it again here */
    
    for (item = opal_list_get_first(&orte_local_children);
         item != opal_list_get_end(&orte_local_children);
         item = opal_list_get_next(item)) {
        child = (orte_odls_child_t*)item;
        
        /* is this child part of the specified job? */
        if (OPAL_EQUAL == opal_dss.compare(&child->name->jobid, &job, ORTE_JOBID)) {
            /* pack the child's vpid - must be done in case rml_uri is NULL */
            if (ORTE_SUCCESS != (rc = opal_dss.pack(buf, &(child->name->vpid), 1, ORTE_VPID))) {
                ORTE_ERROR_LOG(rc);
                return rc;
            }            
            /* pack the contact info */
            if (ORTE_SUCCESS != (rc = opal_dss.pack(buf, &child->rml_uri, 1, OPAL_STRING))) {
                ORTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    
    return ORTE_SUCCESS;
    
}

static void failed_start(orte_odls_job_t *jobdat, orte_exit_code_t exit_code)
{
    opal_list_item_t *item;
    orte_odls_child_t *child;
    
    /* set the state */
    jobdat->state = ORTE_JOB_STATE_FAILED_TO_START;
    
    for (item = opal_list_get_first(&orte_local_children);
         item != opal_list_get_end(&orte_local_children);
         item = opal_list_get_next(item)) {
        child = (orte_odls_child_t*)item;
        if (child->name->jobid == jobdat->jobid) {
            if (ORTE_PROC_STATE_LAUNCHED > child->state ||
                ORTE_PROC_STATE_FAILED_TO_START == child->state) {
                /* this proc never launched - flag that the iof
                 * is complete or else we will hang waiting for
                 * pipes to close that were never opened
                 */
                child->iof_complete = true;
                /* ditto for waitpid */
                child->waitpid_recvd = true;
            }
        }
    }
    OPAL_OUTPUT_VERBOSE((1, orte_errmgr_base.output,
                         "%s errmgr:orcmd: job %s reported incomplete start",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_JOBID_PRINT(jobdat->jobid)));
    return;
}

static void update_local_children(orte_odls_job_t *jobdat, orte_job_state_t jobstate, orte_proc_state_t state)
{
    opal_list_item_t *item;
    orte_odls_child_t *child;
    
    /* update job state */
    jobdat->state = jobstate;
    /* update children */
    for (item = opal_list_get_first(&orte_local_children);
         item != opal_list_get_end(&orte_local_children);
         item = opal_list_get_next(item)) {
        child = (orte_odls_child_t*)item;
        if (jobdat->jobid == child->name->jobid) {
            child->state = state;
        }
    }
}

static void killprocs(orte_jobid_t job, orte_vpid_t vpid)
{
    opal_pointer_array_t cmd;
    orte_proc_t proc;
    int rc;
    
    /* stop local sensors for this job */
    if (ORTE_VPID_WILDCARD == vpid) {
        orte_sensor.stop(job);
    }
    
    if (ORTE_JOBID_WILDCARD == job && ORTE_VPID_WILDCARD == vpid) {
        if (ORTE_SUCCESS != (rc = orte_odls.kill_local_procs(NULL))) {
            ORTE_ERROR_LOG(rc);
        }
        return;
    }
    
    OBJ_CONSTRUCT(&cmd, opal_pointer_array_t);
    OBJ_CONSTRUCT(&proc, orte_proc_t);
    proc.name.jobid = job;
    proc.name.vpid = vpid;
    opal_pointer_array_add(&cmd, &proc);
    /* release the thread before we kill them as this will generate callbacks */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    if (ORTE_SUCCESS != (rc = orte_odls.kill_local_procs(&cmd))) {
        ORTE_ERROR_LOG(rc);
    }
    /* reacquire the thread */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    OBJ_DESTRUCT(&cmd);
    OBJ_DESTRUCT(&proc);
}

static void callback(int status,
                     orte_process_name_t *sender,
                     orcm_pnp_tag_t tag,
                     struct iovec *msg,
                     int count,
                     opal_buffer_t *buf,
                     void *cbdata)
{
    if (NULL != buf) {
        OBJ_RELEASE(buf);
    }
}

static void remote_update(int status,
                          orte_process_name_t *sender,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg,
                          int cnt,
                          opal_buffer_t *buffer,
                          void *cbdata)
{
    orte_std_cntr_t count;
    orte_jobid_t job;
    orte_job_t *jdata;
    orte_vpid_t vpid;
    orte_proc_t *proc;
    orte_proc_state_t state;
    orte_exit_code_t exit_code;
    int rc=ORTE_SUCCESS, ret;
    orte_process_name_t name;
    pid_t pid;

    OPAL_OUTPUT_VERBOSE((5, orte_errmgr_base.output,
                         "%s errmgr:orcmd:receive update proc state command from %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));
    count = 1;
    while (ORTE_SUCCESS == (rc = opal_dss.unpack(buffer, &job, &count, ORTE_JOBID))) {
                    
        OPAL_OUTPUT_VERBOSE((5, orte_errmgr_base.output,
                             "%s errmgr:orcmd:receive got update_proc_state for job %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_JOBID_PRINT(job)));
                    
        name.jobid = job;
        /* get the job object */
        if (NULL == (jdata = orte_get_job_data_object(job))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            return;
        }
        count = 1;
        while (ORTE_SUCCESS == (rc = opal_dss.unpack(buffer, &vpid, &count, ORTE_VPID))) {
            if (ORTE_VPID_INVALID == vpid) {
                /* flag indicates that this job is complete - move on */
                break;
            }
            name.vpid = vpid;
            /* unpack the pid */
            count = 1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &pid, &count, OPAL_PID))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            /* unpack the state */
            count = 1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &state, &count, ORTE_PROC_STATE))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            /* unpack the exit code */
            count = 1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &exit_code, &count, ORTE_EXIT_CODE))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
                        
            OPAL_OUTPUT_VERBOSE((5, orte_errmgr_base.output,
                                 "%s errmgr:orcmd:receive got update_proc_state for vpid %lu state %s exit_code %d",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 (unsigned long)vpid, orte_proc_state_to_str(state), (int)exit_code));
                        
            /* update the state */
            update_state(job, ORTE_JOB_STATE_UNDEF,
                         &name, state, pid, exit_code);
        }
        count = 1;
    }
    if (ORTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER != OPAL_SOS_GET_ERROR_CODE(rc)) {
        ORTE_ERROR_LOG(rc);
    }
}

static void recover_procs(orte_process_name_t *daemon)
{
    orte_job_t *jdt;
    orte_proc_t *proc;
    orte_node_t *node;
    opal_buffer_t bfr;
    int i, rc;

    /* the thread is locked by the caller, so don't do anything here */

    /* if not already done, mark this daemon as down */
    if (NULL != (proc = (orte_proc_t*)opal_pointer_array_get_item(daemon_job->procs, daemon->vpid))) {
        /* remove it from the daemon job array */
        opal_pointer_array_set_item(daemon_job->procs, daemon->vpid, NULL);
        /* correctly track number of alive daemons */
        daemon_job->num_procs--;
        orte_process_info.num_procs--;
        /* get the corresponding node */
        node = proc->node;
        /* maintain accounting */
        OBJ_RELEASE(proc);
    } else {
        /* if it has already been removed, then we need to find the node it was on.
         * this doesn't necessarily correspond to the daemon's vpid, so we have
         * to search the array
         */
    }
    /* mark the node as down so it won't be used in mapping
     * procs to be relaunched
     */
    node->state = ORTE_NODE_STATE;
    node->daemon = NULL;
    /* mark all procs on this node as having terminated */
    for (i=0; i < node->procs->size; i++) {
        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(node->procs, i))) {
            continue;
        }
        /* get the job data object for this process */
        if (NULL == (jdt = orte_get_job_data_object(proc->name.jobid))) {
            /* major problem */
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            continue;
        }
        proc->state = ORTE_PROC_STATE_ABORTED;
        jdt->num_terminated++;
    }

    /* reset the job parameters */
    orte_plm_base_reset_job(jdt);

    /* re-map the job */
    if (ORTE_SUCCESS != (rc = orte_rmaps.map_job(jdt))) {
        ORTE_ERROR_LOG(rc);
        return;
    }         

    /* since we need to retain our own local map of what
     * processes are where, construct a launch msg for
     * this job - but we won't send it anywhere, we'll
     * just locally process it to launch our own procs,
     * if any
     */
    OBJ_CONSTRUCT(&bfr, opal_buffer_t);
    if (ORTE_SUCCESS != (rc = orte_odls.get_add_procs_data(&bfr, jdt->jobid))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&bfr);
        return;
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
}

static void notify_failure(orte_odls_job_t *jobdat, orte_odls_child_t *child, bool restart)
{
    opal_buffer_t *alert;
    int rc;

    alert = OBJ_NEW(opal_buffer_t);

    /* notify all procs of the failure */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(alert, child->name, 1, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    /* send it */
    OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                         "%s NOTIFYING ALL OF %s FAILURE",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(child->name)));
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_ERROR_CHANNEL, NULL,
                                                 ORCM_PNP_TAG_ERRMGR, NULL, 0,
                                                 alert, callback, NULL))) {
        ORTE_ERROR_LOG(rc);
    }
}

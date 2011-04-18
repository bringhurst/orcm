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
#include "opal/dss/dss.h"

#include "orte/threads/threads.h"
#include "orte/util/error_strings.h"
#include "orte/util/name_fns.h"
#include "orte/util/proc_info.h"
#include "orte/util/session_dir.h"
#include "orte/util/show_help.h"
#include "orte/runtime/orte_globals.h"
#include "orte/runtime/orte_wait.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/odls/base/odls_private.h"
#include "orte/mca/plm/plm_types.h"
#include "orte/mca/plm/base/plm_private.h"
#include "orte/mca/rmaps/rmaps.h"
#include "orte/mca/routed/routed.h"
#include "orte/mca/sensor/sensor.h"
#include "orte/orted/orted.h"

#include "mca/pnp/pnp.h"
#include "util/triplets.h"
#include "runtime/runtime.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/errmgr/base/base.h"
#include "orte/mca/errmgr/base/errmgr_private.h"

#include "errmgr_orcmd.h"

/* Local functions */
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
static void delete_proc(orte_job_t *jdata, orte_odls_job_t *jobdat,
                        orte_proc_t *pptr, orte_odls_child_t *child);
/*
 * Module functions: Global
 */
static int init(void);
static int finalize(void);
static void orcmd_abort(int error_code, char *fmt, ...);

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
    orcmd_abort,
    update_state,
    predicted_fault,
    suggest_map_targets,
    ft_event
};

/*
 * Local functions and globals
 */
static orte_thread_ctl_t ctl;
static void notify_state(orte_odls_job_t *jobdat,
                         orte_odls_child_t *child,
                         bool notify_apps);

/************************
 * API Definitions
 ************************/
static int init(void)
{
    /* construct the globals */
    OBJ_CONSTRUCT(&ctl, orte_thread_ctl_t);

    return ORTE_SUCCESS;
}

static int finalize(void)
{
    OBJ_DESTRUCT(&ctl);

    return ORTE_SUCCESS;
}

static void orcmd_abort(int error_code, char *fmt, ...)
{
    va_list arglist;
    
    /* If there was a message, output it */
    va_start(arglist, fmt);
    if( NULL != fmt ) {
        char* buffer = NULL;
        vasprintf( &buffer, fmt, arglist );
        opal_output( 0, "%s", buffer );
        free( buffer );
    }
    va_end(arglist);
    
    kill(getpid(), SIGTERM);
    return;
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
    int rc=ORTE_SUCCESS, i;
    orte_vpid_t null=ORTE_VPID_INVALID;
    orte_app_context_t *app;
    orcm_triplet_t *trp;
    char *stringid = NULL;
    bool one_alive;

    OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                         "%s errmgr:update_state for job %s proc %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_JOBID_PRINT(job),
                         (NULL == proc) ? "NULL" : ORTE_NAME_PRINT(proc)));

    /* protect against threads */
    ORTE_ACQUIRE_THREAD(&ctl);

    /*
     * if orte is trying to shutdown, just let it
     */
    if (orte_finalizing) {
        ORTE_RELEASE_THREAD(&ctl);
        return ORTE_SUCCESS;
    }
    
    if (ORTE_JOBID_IS_DAEMON(job)) {
        /* likely a daemon failed - ignore it */
        ORTE_RELEASE_THREAD(&ctl);
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
            ORTE_RELEASE_THREAD(&ctl);
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
            ORTE_RELEASE_THREAD(&ctl);
            return ORTE_ERR_NOT_FOUND;
        }
        
        switch (jobstate) {
        case ORTE_JOB_STATE_RUNNING:
            /* update all local child states */
            update_local_children(jobdat, jobstate, ORTE_PROC_STATE_RUNNING);
            /* let the scheduler know, but apps don't need to */
            notify_state(jobdat, NULL, false);
            ORTE_RELEASE_THREAD(&ctl);
            return ORTE_SUCCESS;
        case ORTE_JOB_STATE_COMM_FAILED:
            opal_output(0, "%s Job %s failed: %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_JOBID_PRINT(job), orte_job_state_to_str(jobstate));
            /* kill all local procs */
            killprocs(ORTE_JOBID_WILDCARD, ORTE_VPID_WILDCARD);
            /* tell the caller we can't recover */
            ORTE_RELEASE_THREAD(&ctl);
            return ORTE_ERR_UNRECOVERABLE;
        case ORTE_JOB_STATE_HEARTBEAT_FAILED:
            /* should never happen */
            ORTE_RELEASE_THREAD(&ctl);
            return ORTE_SUCCESS;
        case ORTE_JOB_STATE_FAILED_TO_START:
            opal_output(0, "%s Job %s failed: %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_JOBID_PRINT(job), orte_job_state_to_str(jobstate));
            /* mark all local procs for this job as failed to start */
            failed_start(jobdat, exit_code);
            /* let the scheduler know, but apps don't need to know since
             * the job never started and so couldn't announce
             */
            notify_state(jobdat, NULL, false);
            break;
        case ORTE_JOB_STATE_SENSOR_BOUND_EXCEEDED:
            opal_output(0, "%s Job %s failed: %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_JOBID_PRINT(job), orte_job_state_to_str(jobstate));
            /* update all procs in job */
            update_local_children(jobdat, jobstate, ORTE_PROC_STATE_SENSOR_BOUND_EXCEEDED);
            /* order all local procs for this job to be killed */
            killprocs(jobdat->jobid, ORTE_VPID_WILDCARD);
            /* let the scheduler and all apps know */
            notify_state(jobdat, NULL, true);
            break;

        default:
            break;
        }
        /* release thread */
        ORTE_RELEASE_THREAD(&ctl);
        return rc;
    }


    /**** DEAL WITH INDIVIDUAL PROCS ****/

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
        ORTE_RELEASE_THREAD(&ctl);
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
        ORTE_RELEASE_THREAD(&ctl);
        return ORTE_ERR_NOT_FOUND;
    }

    OPAL_OUTPUT_VERBOSE((1, orte_errmgr_base.output,
                         "%s errmgr:orcmd got state %s for proc %s pid %d exit_code %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         orte_proc_state_to_str(state),
                         ORTE_NAME_PRINT(proc), pid, exit_code));
 
    if (ORTE_PROC_STATE_COMM_FAILED != state) {
        child->state = state;
        child->exit_code = exit_code;
        if (0 < pid) {
            child->pid = pid;
        }
        /* must not zero the pid here as we may
         * need it later to issue a kill
         */
    }

    /* if this was a failed comm or heartbeat */
    if (ORTE_PROC_STATE_COMM_FAILED == state ||
        ORTE_PROC_STATE_HEARTBEAT_FAILED == state) {
        /* if this isn't a daemon proc, ignore it */
        if (ORTE_PROC_MY_NAME->jobid != proc->jobid) {
            OPAL_OUTPUT_VERBOSE((5, orte_errmgr_base.output,
                                 "%s Received %s for proc %s - not a daemon",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 orte_proc_state_to_str(state),
                                 ORTE_NAME_PRINT(proc)));
            ORTE_RELEASE_THREAD(&ctl);
            return ORTE_SUCCESS;
        }
        /* if it is our own connection, ignore it - this can
         * happen due to a race condition when we shutdown
         */
        if (ORTE_PROC_MY_NAME->vpid == proc->vpid) {
            ORTE_RELEASE_THREAD(&ctl);
            return ORTE_SUCCESS;
        }
#if 0
        /* delete the route */
        orte_routed.delete_route(proc);
        /* purge the oob */
        orte_rml.purge(proc);
#endif
        /* see if this was a lifeline - only applicable when we
         * are running in developer's mode so that we can cleanly
         * terminate the DVM with a ctrl-c
         */
        if (ORTE_SUCCESS != orte_routed.route_lost(proc)) {
            /* kill our children */
            killprocs(ORTE_JOBID_WILDCARD, ORTE_VPID_WILDCARD);
            /* release the lock */
            ORTE_RELEASE_THREAD(&ctl);
            /* tell us to terminate */
            ORTE_TIMER_EVENT(0, 0, orcm_just_quit);
            /* let the caller know */
            return ORTE_ERR_UNRECOVERABLE;
        }
        /* otherwise, nothing for us to do - let the scheduler
         * direct the recovery
         */
        ORTE_RELEASE_THREAD(&ctl);
        return ORTE_SUCCESS;
    }

    /* find the triplet for this process so we can pretty print messages */
    if (NULL != (trp = orcm_get_triplet_process(proc))) {
        stringid = strdup(trp->string_id);
        ORTE_RELEASE_THREAD(&trp->ctl);
    }

    /***  UPDATE COMMAND FOR A SPECIFIC PROCESS ***/
    if (ORTE_PROC_STATE_SENSOR_BOUND_EXCEEDED == state) {
        if (ORTE_PROC_STATE_UNTERMINATED > child->state) {
            if (NULL != stringid) {
                opal_output(0, "%s Application %s process %s on node %s terminated: sensor bound exceeded",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            stringid,
                            ORTE_NAME_PRINT(proc), orte_process_info.nodename);
                free(stringid);
            } else {
                opal_output(0, "%s Process %s on node %s terminated: sensor bound exceeded",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            ORTE_NAME_PRINT(proc), orte_process_info.nodename);
            }
            /* kill this proc */
            killprocs(proc->jobid, proc->vpid);
        }
        /* we can just return cleanly from here - the death will be
         * recorded when the proc actually dies
         */
        ORTE_RELEASE_THREAD(&ctl);
        return ORTE_SUCCESS;
    }
    
    if (ORTE_PROC_STATE_KILLED_BY_CMD == state) {
        /* since we ordered this proc to die, we just update all the
         * required tracking objects and confirm the death
         */
        if (NULL != stringid) {
            opal_output(0, "%s Application %s process %s on node %s terminated: %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        stringid, ORTE_NAME_PRINT(proc),
                        orte_process_info.nodename,
                        orte_proc_state_to_str(state));
            free(stringid);
        } else {
            opal_output(0, "%s Process %s on node %s terminated: %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(proc),
                        orte_process_info.nodename,
                        orte_proc_state_to_str(state));
        }
        /* this proc is dead */
        child->pid = 0;
        child->alive = false;
        /* let people know it was successfully killed */
        notify_state(jobdat, child, true);
        goto CHECK_ALIVE;
    }

    if (ORTE_PROC_STATE_TERMINATED < state) {
        if (ORTE_PROC_STATE_ABORTED_BY_SIG == state) {
            if (NULL != stringid) {
                opal_output(0, "%s Application %s process %s on node %s terminated: %s(%s)",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            stringid, ORTE_NAME_PRINT(proc),
                            orte_process_info.nodename,
                            orte_proc_state_to_str(state),
                            orte_proc_exit_code_to_signal(exit_code));
                free(stringid);
            } else {
                opal_output(0, "%s Process %s on node %s terminated: %s(%s)",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            ORTE_NAME_PRINT(proc),
                            orte_process_info.nodename,
                            orte_proc_state_to_str(state),
                            orte_proc_exit_code_to_signal(exit_code));
            }
        } else {
            if (NULL != stringid) {
                opal_output(0, "%s Application %s process %s on node %s terminated: %s",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            stringid, ORTE_NAME_PRINT(proc),
                            orte_process_info.nodename,
                            orte_proc_state_to_str(state));
                free(stringid);
            } else {
                opal_output(0, "%s Process %s on node %s terminated: %s",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            ORTE_NAME_PRINT(proc),
                            orte_process_info.nodename,
                            orte_proc_state_to_str(state));
            }
        }
        /* this proc is dead */
        child->pid = 0;
        child->alive = false;
        /* let the scheduler and all apps know */
        notify_state(jobdat, child, true);
        goto CHECK_ALIVE;
    }    
    
    /* get here if proc didn't fail in any manner - advise scheduler of the state */
    notify_state(jobdat, child, false);

    if (ORTE_PROC_STATE_UNTERMINATED > state) {
        ORTE_RELEASE_THREAD(&ctl);
        return ORTE_SUCCESS;
    }

    /* only other state is terminated - see if anyone is left alive */
    OPAL_OUTPUT_VERBOSE((5, orte_errmgr_base.output,
                         "%s errmgr:orcmd all procs in %s terminated normally",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_JOBID_PRINT(jobdat->jobid)));

 CHECK_ALIVE:
    /* remove all of this job's children from the global list - do not lock
     * the thread as we are already locked
     */
    one_alive = false;
    for (item = opal_list_get_first(&orte_local_children);
         item != opal_list_get_end(&orte_local_children);
         item = next) {
        child = (orte_odls_child_t*)item;
        next = opal_list_get_next(item);
            
        if (jobdat->jobid == child->name->jobid) {
            if (!child->notified) {
                one_alive = true;
            } else {
                OPAL_OUTPUT_VERBOSE((7, orte_errmgr_base.output,
                                     "%s REMOVING CHILD %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_NAME_PRINT(child->name)));
                opal_list_remove_item(&orte_local_children, &child->super);
                OBJ_RELEASE(child);
                jobdat->num_local_procs--;
            }
        }
    }
    if (!one_alive) {
        /* ensure the job's local session directory tree is removed */
        orte_session_dir_cleanup(jobdat->jobid);
        
        /* remove this job from our local job data since it is complete */
        opal_list_remove_item(&orte_local_jobdata, &jobdat->super);
        OBJ_RELEASE(jobdat);
    }
        
    ORTE_RELEASE_THREAD(&ctl);
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
                /* note that it isn't alive */
                child->alive = false;
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

    /* update children, except when they were killed by cmd - there
     * is a special race condition that can cause us to overwrite
     * that state when the proc is already dead
     */
    for (item = opal_list_get_first(&orte_local_children);
         item != opal_list_get_end(&orte_local_children);
         item = opal_list_get_next(item)) {
        child = (orte_odls_child_t*)item;
        if (jobdat->jobid == child->name->jobid &&
            ORTE_PROC_STATE_KILLED_BY_CMD != child->state) {
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
        /* release the thread before we kill them as this will generate callbacks */
        ORTE_RELEASE_THREAD(&ctl);
        if (ORTE_SUCCESS != (rc = orte_odls.kill_local_procs(NULL))) {
            ORTE_ERROR_LOG(rc);
        }
        /* reacquire the threads */
        ORTE_ACQUIRE_THREAD(&ctl);
        return;
    }
    
    OBJ_CONSTRUCT(&cmd, opal_pointer_array_t);
    OBJ_CONSTRUCT(&proc, orte_proc_t);
    proc.name.jobid = job;
    proc.name.vpid = vpid;
    opal_pointer_array_add(&cmd, &proc);
    /* release the thread before we kill them as this will generate callbacks */
    ORTE_RELEASE_THREAD(&ctl);
    if (ORTE_SUCCESS != (rc = orte_odls.kill_local_procs(&cmd))) {
        ORTE_ERROR_LOG(rc);
    }
    /* reacquire the thread */
    ORTE_ACQUIRE_THREAD(&ctl);
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


static void notify_state(orte_odls_job_t *jobdat,
                         orte_odls_child_t *child,
                         bool notify_apps)
{
    opal_buffer_t *alert, *notify;
    int rc;
    uint8_t flag;
    orte_process_name_t name;
    opal_list_item_t *item;
    orte_odls_child_t *ch;

    alert = OBJ_NEW(opal_buffer_t);
    if (notify_apps) {
        notify = OBJ_NEW(opal_buffer_t);
    }

    if (NULL == child) {
        /* record each child */
        for (item = opal_list_get_first(&orte_local_children);
             item != opal_list_get_end(&orte_local_children);
             item = opal_list_get_next(item)) {
            ch = (orte_odls_child_t*)item;

            if (ch->name->jobid == jobdat->jobid) {
                /* record the failed child */
                if (ORTE_SUCCESS != (rc = opal_dss.pack(alert, ch->name, 1, ORTE_NAME))) {
                    ORTE_ERROR_LOG(rc);
                    return;
                }
                if (ORTE_SUCCESS != (rc = opal_dss.pack(alert, &ch->pid, 1, OPAL_PID))) {
                    ORTE_ERROR_LOG(rc);
                    return;
                }
                if (ORTE_SUCCESS != (rc = opal_dss.pack(alert, &ch->state, 1, ORTE_PROC_STATE))) {
                    ORTE_ERROR_LOG(rc);
                    return;
                }
                if (ORTE_SUCCESS != (rc = opal_dss.pack(alert, &ch->exit_code, 1, ORTE_EXIT_CODE))) {
                    ORTE_ERROR_LOG(rc);
                    return;
                }
                if (notify_apps) {
                    /* save a record for apps to get */
                    if (ORTE_SUCCESS != (rc = opal_dss.pack(notify, ch->name, 1, ORTE_NAME))) {
                        ORTE_ERROR_LOG(rc);
                        return;
                    }
                    /* flag that notification sent */
                    ch->notified = true;
                }
            }
        }
    } else {
        /* record the failed child */
        if (ORTE_SUCCESS != (rc = opal_dss.pack(alert, child->name, 1, ORTE_NAME))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        if (ORTE_SUCCESS != (rc = opal_dss.pack(alert, &child->pid, 1, OPAL_PID))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        if (ORTE_SUCCESS != (rc = opal_dss.pack(alert, &child->state, 1, ORTE_PROC_STATE))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        if (ORTE_SUCCESS != (rc = opal_dss.pack(alert, &child->exit_code, 1, ORTE_EXIT_CODE))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        if (notify_apps) {
            /* save a record for apps to get */
            if (ORTE_SUCCESS != (rc = opal_dss.pack(notify, child->name, 1, ORTE_NAME))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            /* flag that notification sent */
            child->notified = true;
        }
    }

    if (notify_apps) {
        OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                             "%s sending failure notice for %s to all apps",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (NULL == child) ? ORTE_JOBID_PRINT(jobdat->jobid) : ORTE_NAME_PRINT(child->name)));
        /* send it to all apps */
        if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_ERROR_CHANNEL, NULL,
                                                     ORCM_PNP_TAG_ERRMGR, NULL, 0,
                                                     notify, callback, NULL))) {
            ORTE_ERROR_LOG(rc);
        }
    }
    /* send it to the scheduler */
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_ERROR_CHANNEL, NULL,
                                                 ORCM_PNP_TAG_UPDATE_STATE, NULL, 0,
                                                 alert, callback, NULL))) {
        ORTE_ERROR_LOG(rc);
    }
}

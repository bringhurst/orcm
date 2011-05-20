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
#include "orte/mca/rml/rml.h"
#include "orte/mca/odls/odls.h"
#include "orte/mca/odls/base/odls_private.h"
#include "orte/mca/plm/plm_types.h"
#include "orte/mca/plm/base/plm_private.h"
#include "orte/mca/rmaps/rmaps.h"
#include "orte/mca/routed/routed.h"
#include "orte/mca/sensor/sensor.h"
#include "orte/orted/orted.h"
#include "orte/mca/ess/ess.h"

#include "mca/pnp/pnp.h"
#include "util/triplets.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/errmgr/base/base.h"
#include "orte/mca/errmgr/base/errmgr_private.h"

#include "errmgr_sched.h"

/*
 * Module functions: Global
 */
static int init(void);
static int finalize(void);
static void sched_abort(int error_code, char *fmt, ...);

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
 * SCHED module
 ******************/
orte_errmgr_base_module_t orte_errmgr_orcmsched_module = {
    init,
    finalize,
    orte_errmgr_base_log,
    sched_abort,
    update_state,
    predicted_fault,
    suggest_map_targets,
    ft_event
};

/*
 * Local functions and globals
 */
static orte_thread_ctl_t ctl;
static orte_job_t *daemon_job=NULL;
static void recover_procs(orte_process_name_t *daemon_that_failed);
static void remote_update(int status,
                          orte_process_name_t *sender,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg,
                          int count,
                          opal_buffer_t *buf,
                          void *cbdata);
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


/************************
 * API Definitions
 ************************/
static int init(void)
{
    int rc;

    /* construct the globals */
    OBJ_CONSTRUCT(&ctl, orte_thread_ctl_t);

    /* get the daemon job object */
    if (NULL == (daemon_job = orte_get_job_data_object(ORTE_PROC_MY_NAME->jobid))) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        return ORTE_ERR_NOT_FOUND;
    }

    /* setup to recv errmgr notifications from our dvm */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_receive("orcmd", "0.1", "alpha",
                                                        ORCM_PNP_ERROR_CHANNEL,
                                                        ORCM_PNP_TAG_UPDATE_STATE,
                                                        remote_update, NULL))) {
        ORTE_ERROR_LOG(rc);
    }
    return rc;
}

static int finalize(void)
{
    OBJ_DESTRUCT(&ctl);

    orcm_pnp.cancel_receive("orcmd", "0.1", "alpha", ORCM_PNP_SYS_CHANNEL, ORCM_PNP_TAG_ERRMGR);
    return ORTE_SUCCESS;
}

static void sched_abort(int error_code, char *fmt, ...)
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
    
    if (orte_abnormal_term_ordered) {
        /* only send SIGTERM to ourselves once as
         * we otherwise can get into an infinite loop
         * while trying to abnormally terminate
         */
        orte_ess.abort(error_code, false);
    } else {
        orte_abnormal_term_ordered = true;
        kill(getpid(), SIGTERM);
    }
    return;
}

static int update_state(orte_jobid_t job,
                        orte_job_state_t jobstate,
                        orte_process_name_t *proc,
                        orte_proc_state_t state,
                        pid_t pid,
                        orte_exit_code_t exit_code)
{
    int rc=ORTE_SUCCESS, i;
    orte_app_context_t *app;
    orte_node_t *node;
    orte_proc_t *pptr, *daemon, *pptr2;
    opal_buffer_t *notify;
    orcm_triplet_t *trp;
    orcm_source_t *src;
    bool procs_recovered;
    orte_job_t *jdt;
    uint16_t jfam;
    bool send_msg;

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
    
    /***   UPDATE COMMAND FOR A JOB   ***/
    if (NULL == proc) {
        /* should only get this if a daemon restarted and we need
         * to check for procs waiting to migrate
         */
        if (ORTE_JOB_STATE_PROCS_MIGRATING != jobstate) {
            /* we should never get this situation */
            opal_output(0, "%s UNKNOWN JOB ERROR ",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), orte_job_state_to_str(jobstate));
            ORTE_RELEASE_THREAD(&ctl);
            return ORTE_ERROR;
        }
        /* cycle thru all known jobs looking for those with procs
         * awaiting resources to migrate
         */
        for (i=0; i < orte_job_data->size; i++) {
            if (NULL == (jdt = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
                continue;
            }
            if (ORTE_JOB_STATE_PROCS_MIGRATING != jdt->state) {
                continue;
            }
            /* reset the job */
            orte_plm_base_reset_job(jdt);

            /* map the job again */
            if (ORTE_SUCCESS != (rc = orte_rmaps.map_job(jdt))) {
                ORTE_ERROR_LOG(rc);
                continue;
            }
            /* launch any procs that could be mapped - note that not
             * all procs that were waiting for migration may have
             * been successfully mapped, so this could in fact
             * result in no action by the daemons
             */
            notify = OBJ_NEW(opal_buffer_t);
            /* indicate the target DVM */
            jfam = ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid);
            opal_dss.pack(notify, &jfam, 1, OPAL_UINT16);

            /* get the launch data */
            if (ORTE_SUCCESS != (rc = orte_odls.get_add_procs_data(notify, jdt->jobid))) {
                ORTE_ERROR_LOG(rc);
                OBJ_RELEASE(notify);
                ORTE_RELEASE_THREAD(&ctl);
                return ORTE_SUCCESS;
            }
            /* send it to the daemons */
            if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_SYS_CHANNEL,
                                                         NULL, ORCM_PNP_TAG_COMMAND,
                                                         NULL, 0, notify, cbfunc, NULL))) {
                ORTE_ERROR_LOG(rc);
            }
        }
        ORTE_RELEASE_THREAD(&ctl);
        return ORTE_SUCCESS;
    }


    /**** DEAL WITH INDIVIDUAL PROCS ****/

    OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                         "%s errmgr:sched got state %s for proc %s pid %d exit_code %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         orte_proc_state_to_str(state),
                         ORTE_NAME_PRINT(proc), pid, exit_code));
 
    /* if this was a failed comm or heartbeat */
    if (ORTE_PROC_STATE_COMM_FAILED == state) {
        /* ignore this */
        ORTE_RELEASE_THREAD(&ctl);
        return ORTE_SUCCESS;
    }

    if (ORTE_PROC_STATE_HEARTBEAT_FAILED == state) {
        /* get the proc object for this daemon */
        if (NULL == (daemon = (orte_proc_t*)opal_pointer_array_get_item(daemon_job->procs, proc->vpid))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            ORTE_RELEASE_THREAD(&ctl);
            return ORTE_ERR_NOT_FOUND;
        }
        /* ensure that the heartbeat system knows to ignore this proc
         * from this point forward
         */
        daemon->beat = 0;
        /* if we have already heard about this proc, ignore repeats */
        if (ORTE_PROC_STATE_HEARTBEAT_FAILED == daemon->state) {
            /* already heard */
            ORTE_RELEASE_THREAD(&ctl);
            return ORTE_SUCCESS;
        }
#if 0
        /* delete the route */
        orte_routed.delete_route(proc);
        /* purge the oob */
        orte_rml.purge(proc);
#endif
        /* get the triplet/source and mark this source as "dead" */
        if (NULL == (trp = orcm_get_triplet_stringid("orcmd:0.1:alpha"))) {
            opal_output(0, "%s CANNOT FIND DAEMON TRIPLET",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            ORTE_RELEASE_THREAD(&ctl);
            return ORTE_ERR_NOT_FOUND;
        }
        if (NULL == (src = orcm_get_source(trp, proc, false))) {
            opal_output(0, "%s DAEMON %s IS UNKNOWN SOURCE",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(proc));
            ORTE_RELEASE_THREAD(&trp->ctl);
            ORTE_RELEASE_THREAD(&ctl);
            return ORTE_ERR_NOT_FOUND;
        }
        src->alive = false;
        ORTE_RELEASE_THREAD(&src->ctl);
        ORTE_RELEASE_THREAD(&trp->ctl);

        /* notify all apps immediately */
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, proc->vpid))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            ORTE_RELEASE_THREAD(&ctl);
            return ORTE_ERR_NOT_FOUND;
        }
        notify = OBJ_NEW(opal_buffer_t);
        send_msg = false;
        for (i=0; i < node->procs->size; i++) {
            if (NULL == (pptr = (orte_proc_t*)opal_pointer_array_get_item(node->procs, i))) {
                continue;
            }
            if (ORTE_SUCCESS != (rc = opal_dss.pack(notify, &pptr->name, 1, ORTE_NAME))) {
                ORTE_ERROR_LOG(rc);
                ORTE_RELEASE_THREAD(&ctl);
                return rc;
            }
            /* reset the proc stats */
            OBJ_DESTRUCT(&pptr->stats);
            OBJ_CONSTRUCT(&pptr->stats, opal_pstats_t);
            /* since we added something, need to send msg */
            send_msg = true;
        }
        if (send_msg) {
            /* send it to all apps */
            if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_ERROR_CHANNEL, NULL,
                                                         ORCM_PNP_TAG_ERRMGR, NULL, 0,
                                                         notify, cbfunc, NULL))) {
                ORTE_ERROR_LOG(rc);
            }
        } else {
            OBJ_RELEASE(notify);
        }
        /* reset the node stats */
        OBJ_DESTRUCT(&node->stats);
        OBJ_CONSTRUCT(&node->stats, opal_node_stats_t);
        /* record that the daemon died */
        daemon->state = state;
        daemon->exit_code = exit_code;
        daemon->pid = 0;
        /* reset the daemon stats */
        OBJ_DESTRUCT(&daemon->stats);
        OBJ_CONSTRUCT(&daemon->stats, opal_pstats_t);
        node = daemon->node;
        if (NULL == node) {
            opal_output(0, "%s Detected failure of daemon %s on unknown node",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(proc));
            /* can't do anything further */
            ORTE_RELEASE_THREAD(&ctl);
            return ORTE_SUCCESS;            
        } else {
            opal_output(0, "%s Detected failure of daemon %s on node %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(proc),
                        (NULL == node->name) ? "UNKNOWN" : node->name);
        }
        /* see if any usable daemons are left alive */
        procs_recovered = false;
        for (i=2; i < daemon_job->procs->size; i++) {
            if (NULL == (pptr = (orte_proc_t*)opal_pointer_array_get_item(daemon_job->procs, i))) {
                continue;
            }
            if (ORTE_PROC_STATE_UNTERMINATED < pptr->state) {
                continue;
            }
            /* at least one alive! recover procs from the failed one */
            recover_procs(proc);
            procs_recovered = true;
            break;
        }
        if (!procs_recovered) {
            daemon->node = NULL;
            node->state = ORTE_NODE_STATE_DOWN;
            node->daemon = NULL;
            /* mark all procs on this node as having terminated */
            for (i=0; i < node->procs->size; i++) {
                if (NULL == (pptr = (orte_proc_t*)opal_pointer_array_get_item(node->procs, i))) {
                    continue;
                }
                /* get the job data object for this process */
                if (NULL == (jdt = orte_get_job_data_object(pptr->name.jobid))) {
                    /* major problem */
                    opal_output(0, "%s COULD NOT GET JOB OBJECT FOR PROC %s(%d): state %s",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                ORTE_NAME_PRINT(&pptr->name), i,
                                orte_proc_state_to_str(pptr->state));
                    continue;
                }
                if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdt->apps, pptr->app_idx))) {
                    continue;
                }
                OPAL_OUTPUT_VERBOSE((3, orte_errmgr_base.output,
                                     "%s REMOVING PROC %s FROM NODE %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_NAME_PRINT(&pptr->name), node->name));
                app->num_procs--;
                opal_pointer_array_set_item(jdt->procs, pptr->name.vpid, NULL);
                OBJ_RELEASE(pptr);
                /* clean it off the node */
                opal_pointer_array_set_item(node->procs, i, NULL);
                node->num_procs--;
                /* maintain acctg */
                OBJ_RELEASE(pptr);
                /* see if job is empty */
                jdt->num_terminated++;
                if (jdt->num_procs <= jdt->num_terminated) {
                    OPAL_OUTPUT_VERBOSE((3, orte_errmgr_base.output,
                                         "%s REMOVING JOB %s FROM ACTIVE ARRAY",
                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                         ORTE_JOBID_PRINT(jdt->jobid)));
                    opal_pointer_array_set_item(orte_job_data, ORTE_LOCAL_JOBID(jdt->jobid), NULL);
                    OBJ_RELEASE(jdt);
                }
            }
        }
        ORTE_RELEASE_THREAD(&ctl);
        return ORTE_SUCCESS;
    }

    if (ORTE_PROC_STATE_RESTARTED == state) {
        OPAL_OUTPUT_VERBOSE((3, orte_errmgr_base.output,
                             "%s RESTART OF DAEMON %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(proc)));
        /* get the proc object for this daemon */
        if (NULL == (daemon = (orte_proc_t*)opal_pointer_array_get_item(daemon_job->procs, proc->vpid))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            ORTE_RELEASE_THREAD(&ctl);
            return ORTE_ERR_NOT_FOUND;
        }
        /* if apps were on that node, notify all apps immediately that
         * those procs have failed
         */
        if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, proc->vpid))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            ORTE_RELEASE_THREAD(&ctl);
            return ORTE_ERR_NOT_FOUND;
        }
        notify = OBJ_NEW(opal_buffer_t);
        send_msg = false;
        for (i=0; i < node->procs->size; i++) {
            if (NULL == (pptr = (orte_proc_t*)opal_pointer_array_get_item(node->procs, i))) {
                continue;
            }
            if (ORTE_SUCCESS != (rc = opal_dss.pack(notify, &pptr->name, 1, ORTE_NAME))) {
                ORTE_ERROR_LOG(rc);
                ORTE_RELEASE_THREAD(&ctl);
                return rc;
            }
            /* since we added something, we need to send msg */
            send_msg = true;
            /* remove the proc from the app so that it will get
             * restarted when we re-activate the config
             */
            if (NULL == (jdt = orte_get_job_data_object(pptr->name.jobid))) {
                continue;
            }
            if (NULL == (app = (orte_app_context_t*)opal_pointer_array_get_item(jdt->apps, pptr->app_idx))) {
                continue;
            }
            OPAL_OUTPUT_VERBOSE((3, orte_errmgr_base.output,
                                 "%s REMOVING PROC %s FROM NODE %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(&pptr->name), node->name));
            app->num_procs--;
            opal_pointer_array_set_item(jdt->procs, pptr->name.vpid, NULL);
            OBJ_RELEASE(pptr);
            /* clean it off the node */
            opal_pointer_array_set_item(node->procs, i, NULL);
            node->num_procs--;
            /* maintain acctg */
            OBJ_RELEASE(pptr);
            /* see if job is empty */
            jdt->num_terminated++;
            if (jdt->num_procs <= jdt->num_terminated) {
                OPAL_OUTPUT_VERBOSE((3, orte_errmgr_base.output,
                                     "%s REMOVING JOB %s FROM ACTIVE ARRAY",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_JOBID_PRINT(jdt->jobid)));
                opal_pointer_array_set_item(orte_job_data, ORTE_LOCAL_JOBID(jdt->jobid), NULL);
                OBJ_RELEASE(jdt);
            }
        }
        if (send_msg) {
            /* send it to all apps */
            if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_ERROR_CHANNEL, NULL,
                                                         ORCM_PNP_TAG_ERRMGR, NULL, 0,
                                                         notify, cbfunc, NULL))) {
                ORTE_ERROR_LOG(rc);
            }
        } else {
            OBJ_RELEASE(notify);
        }
        /* reset the node stats */
        OBJ_DESTRUCT(&node->stats);
        OBJ_CONSTRUCT(&node->stats, opal_node_stats_t);
        /* reset the daemon stats */
        OBJ_DESTRUCT(&daemon->stats);
        OBJ_CONSTRUCT(&daemon->stats, opal_pstats_t);
        /* don't restart procs - we'll do that later after
         * we allow time for multiple daemons to restart
         */
        ORTE_RELEASE_THREAD(&ctl);
        return ORTE_SUCCESS;
    }

    /* to arrive here is an error */
    opal_output(0, "%s GOT UNRECOGNIZED STATE %s FOR PROC %s",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                orte_proc_state_to_str(state),
                ORTE_NAME_PRINT(proc));
    return ORTE_ERROR;

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
/* failure notifications come here */
static void remote_update(int status,
                          orte_process_name_t *sender,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg,
                          int count,
                          opal_buffer_t *buffer,
                          void *cbdata)
{
    int rc, n, k, cnt;
    orte_process_name_t name;
    uint8_t flag;
    orte_job_t *jdata;
    orte_proc_t *proc, *pptr;
    orte_node_t *node;
    orte_app_context_t *app;
    opal_buffer_t *bfr;
    orte_proc_state_t state;
    orte_exit_code_t exit_code;
    pid_t pid;
    bool restart_reqd, job_released, job_done;
    uint16_t jfam;

    OPAL_OUTPUT_VERBOSE((5, orte_errmgr_base.output,
                         "%s errmgr:sched:receive proc state notification from %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));

    /* get the node object for the sender */
    if (NULL == (node = (orte_node_t*)opal_pointer_array_get_item(orte_node_pool, sender->vpid))) {
        opal_output(0, "%s CANNOT FIND NODE FOR DAEMON %s",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_NAME_PRINT(sender));
        return;
    }

    /* unpack the names of the procs */
    restart_reqd = false;
    n=1;
    while (ORTE_SUCCESS == (rc = opal_dss.unpack(buffer, &name, &n, ORTE_NAME))) {

        OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                             "%s GOT UPDATE FOR %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&name)));

        /* unpack the pid of the proc */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &pid, &n, OPAL_PID))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        /* unpack the state of the proc */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &state, &n, ORTE_PROC_STATE))) {
            ORTE_ERROR_LOG(rc);
            return;
        }
        /* unpack the exit_code of the proc */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &exit_code, &n, ORTE_EXIT_CODE))) {
            ORTE_ERROR_LOG(rc);
            return;
        }

        /* get the job object for this proc */
        if (NULL == (jdata = orte_get_job_data_object(name.jobid))) {
            /* BIG problem*/
            opal_output(0, "%s errmgr:sched JOB %s NOT FOUND",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_JOBID_PRINT(name.jobid));
            return;
        }

        /* get the proc object */
        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, name.vpid))) {
            /* unknown proc - race condition when killing a proc on cmd */
            OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                                 "%s MISSING PROC %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(&name)));
            continue;
        }
        /* update data */
        proc->pid = pid;
        OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                             "%s CHANGING STATE OF PROC %s FROM %s TO %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&name),
                             orte_proc_state_to_str(proc->state),
                             orte_proc_state_to_str(state)));
        proc->state = state;
        proc->exit_code = exit_code;
        /* if the proc has failed, mark the job for restart unless
         * it was killed by our own cmd
         */
        if (ORTE_PROC_STATE_UNTERMINATED < state) {
            /* reset the stats */
            OBJ_DESTRUCT(&proc->stats);
            OBJ_CONSTRUCT(&proc->stats, opal_pstats_t);
            if (ORTE_PROC_STATE_KILLED_BY_CMD == state) {
                /* this is a response to our killing a proc - remove it
                 * from the system
                 */
                opal_pointer_array_set_item(jdata->procs, name.vpid, NULL);
                jdata->num_procs--;
                /* clean it off of the node */
                for (k=0; k < node->procs->size; k++) {
                    if (NULL == (pptr = (orte_proc_t*)opal_pointer_array_get_item(node->procs, k))) {
                        continue;
                    }
                    if (pptr->name.jobid == proc->name.jobid &&
                        pptr->name.vpid == proc->name.vpid) {
                        /* found it */
                        OPAL_OUTPUT_VERBOSE((7, orte_errmgr_base.output,
                                             "%s REMOVING ENTRY %d FOR PROC %s FROM NODE %s",
                                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), k,
                                             ORTE_NAME_PRINT(&proc->name),
                                             ORTE_VPID_PRINT(sender->vpid)));
                        opal_pointer_array_set_item(node->procs, k, NULL);
                        node->num_procs--;
                        /* maintain acctg */
                        OBJ_RELEASE(proc);
                        break;
                    }
                }
                /* release the object */
                OBJ_RELEASE(proc);
                /* if the job is now empty, or if the only procs remaining are stopped
                 * due to exceeding restart (and thus cannot run), remove it too
                 */
                if (0 == jdata->num_procs) {
                    opal_pointer_array_set_item(orte_job_data, ORTE_LOCAL_JOBID(jdata->jobid), NULL);
                    OBJ_RELEASE(jdata);
                } else {
                    job_done = true;
                    for (k=0; k < jdata->procs->size; k++) {
                        if (NULL == (pptr = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, k))) {
                            continue;
                        }
                        OPAL_OUTPUT_VERBOSE((3, orte_errmgr_base.output,
                                             "%s CHECKING PROC %s STATE %s",
                                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                             ORTE_NAME_PRINT(&pptr->name),
                                             orte_proc_state_to_str(pptr->state)));
                        if (pptr->state < ORTE_PROC_STATE_UNTERMINATED ||
                            ORTE_PROC_STATE_CANNOT_RESTART != pptr->state) {
                            job_done = false;
                            break;
                        }
                    }
                    if (job_done) {
                        opal_pointer_array_set_item(orte_job_data, ORTE_LOCAL_JOBID(jdata->jobid), NULL);
                        OBJ_RELEASE(jdata);
                    }
                }
            } else {
                OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                                     "%s FLAGGING JOB %s AS CANDIDATE FOR RESTART",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_JOBID_PRINT(jdata->jobid)));
                jdata->state = ORTE_JOB_STATE_RESTART;
                /* flag that at least one job requires restart */
                restart_reqd = true;
            }
        }
        /* prep for next round */
        n=1;
    }
    if (ORCM_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        ORTE_ERROR_LOG(rc);
    }

    /* if restart not reqd, nothing more to do */
    if (!restart_reqd) {
        OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                             "%s NO RESTARTS REQUIRED",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        return;
    }

    /* cycle thru the array of jobs looking for those requiring restart */
    for (n=1; n < orte_job_data->size; n++) {
        if (NULL == (jdata = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, n))) {
            continue;
        }
        if (ORTE_JOB_STATE_RESTART != jdata->state) {
            continue;
        }
        OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                             "%s JOB %s CANDIDATE FOR RESTART",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_JOBID_PRINT(jdata->jobid)));
        /* find the proc that needs restarting */
        restart_reqd = false;
        job_released = false;
        for (cnt=0; cnt < jdata->procs->size; cnt++) {
            if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, cnt))) {
                continue;
            }
            if (ORTE_PROC_STATE_UNTERMINATED < proc->state &&
                ORTE_PROC_STATE_KILLED_BY_CMD != proc->state) {
                /* get the app for this proc */
                app = (orte_app_context_t*)opal_pointer_array_get_item(jdata->apps, proc->app_idx);
                if (NULL == app) {
                    opal_output(0, "%s UNKNOWN APP", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                    continue;
                }

                /* check the number of restarts to see if the limit has been reached */
                if (app->max_restarts < 0 ||
                    proc->restarts < app->max_restarts) {
                    OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                                         "%s FLAGGING PROC %s FOR RESTART",
                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                         ORTE_NAME_PRINT(&proc->name)));
                    /* flag the proc for restart */
                    proc->state = ORTE_PROC_STATE_RESTART;
                    restart_reqd = true;
                    /* adjust accounting */
                    jdata->num_terminated++;
                    /* increment the restart counter since the proc will be restarted */
                    proc->restarts++;
                } else {
                    /* limit reached - don't restart it */
                    OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                                         "%s PROC %s AT LIMIT - CANNOT RESTART",
                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                         ORTE_NAME_PRINT(&proc->name)));
                    /* leave the proc in the system so users can see that it
                     * reached the restart limit
                     */
                    proc->state = ORTE_PROC_STATE_CANNOT_RESTART;
                    proc->pid = 0;
                    /* increment his restarts this once so it shows as too high */
                    proc->restarts++;
                    /* adjust accounting */
                    jdata->num_procs--;
                    jdata->num_terminated++;
                    /* clean it off of the node */
                    if (NULL == (node = proc->node)) {
                        continue;
                    }
                    for (k=0; k < node->procs->size; k++) {
                        if (NULL == (pptr = (orte_proc_t*)opal_pointer_array_get_item(node->procs, k))) {
                            continue;
                        }
                        if (pptr == proc) {
                            /* found it */
                            opal_pointer_array_set_item(node->procs, k, NULL);
                            node->num_procs--;
                            /* maintain acctg */
                            OBJ_RELEASE(proc);
                            proc->node = NULL;
                            break;
                        }
                    }
                }
            }
        }
        /* if the job was released, then move on */
        if (job_released) {
            continue;
        }
        /* if no procs require restart, then move on to next job */
        if (!restart_reqd) {
            jdata->state = ORTE_JOB_STATE_RUNNING;  /* reset this */
            continue;
        }

        /* reset the job */
        orte_plm_base_reset_job(jdata);

        /* the resilient mapper will automatically avoid restarting the
         * proc on its former node
         */

        /* map the job again */
        if (ORTE_SUCCESS != (rc = orte_rmaps.map_job(jdata))) {
            ORTE_ERROR_LOG(rc);
            return;
        }         

        bfr = OBJ_NEW(opal_buffer_t);
        /* indicate the target DVM */
        jfam = ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid);
        opal_dss.pack(bfr, &jfam, 1, OPAL_UINT16);

        /* get the launch data */
        if (ORTE_SUCCESS != (rc = orte_odls.get_add_procs_data(bfr, jdata->jobid))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(bfr);
            return;
        }
        /* send it to the daemons */
        if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_SYS_CHANNEL,
                                                     NULL, ORCM_PNP_TAG_COMMAND,
                                                     NULL, 0, bfr, cbfunc, NULL))) {
            ORTE_ERROR_LOG(rc);
        }
    }
}

static void recover_procs(orte_process_name_t *daemon)
{
    orte_job_t *jdt;
    orte_proc_t *proc;
    orte_node_t *node=NULL;
    int i, rc;
    opal_buffer_t *bfr;
    uint16_t jfam;

    /* the thread is locked by the caller, so don't do anything here */

    OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                         "%s ATTEMPTING TO RECOVER PROCS FROM DAEMON %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(daemon)));

    /* if not already done, mark this daemon as down */
    if (NULL != (proc = (orte_proc_t*)opal_pointer_array_get_item(daemon_job->procs, daemon->vpid))) {
        /* correctly track number of alive daemons */
        daemon_job->num_terminated++;
        orte_process_info.num_procs--;
        /* get the corresponding node */
        node = proc->node;
        /* maintain accounting */
        OBJ_RELEASE(proc);
        proc->node = NULL;
    } else {
        /* if it has already been removed, then we need to find the node it was on.
         * this doesn't necessarily correspond to the daemon's vpid, so we have
         * to search the array
         */
        opal_output(0, "RECOVER PROCS - MISSING NODE");
        return;
    }
    /* mark the node as down so it won't be used in mapping
     * procs to be relaunched
     */
    OPAL_OUTPUT_VERBOSE((2, orte_errmgr_base.output,
                         "%s MARKING NODE %s DOWN",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         node->name));

    node->state = ORTE_NODE_STATE_DOWN;
    node->daemon = NULL;
    /* mark all procs on this node as having terminated */
    for (i=0; i < node->procs->size; i++) {
        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(node->procs, i))) {
            continue;
        }
        /* get the job data object for this process */
        if (NULL == (jdt = orte_get_job_data_object(proc->name.jobid))) {
            /* major problem */
            opal_output(0, "%s COULD NOT GET JOB OBJECT FOR PROC %s(%d): state %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&proc->name), i,
                        orte_proc_state_to_str(proc->state));
            continue;
        }
        /* since the proc failed for reasons other than its own, this restart
         * does not count against its total - so mark it for restart
         */
        proc->state = ORTE_PROC_STATE_RESTART;
        proc->pid = 0;
        jdt->state = ORTE_JOB_STATE_RESTART;
        /* adjust the num terminated so that acctg works right */
        jdt->num_terminated++;
    }

    /* now cycle thru the jobs and restart all those that were flagged */
    for (i=0; i < orte_job_data->size; i++) {
        if (NULL == (jdt = (orte_job_t*)opal_pointer_array_get_item(orte_job_data, i))) {
            continue;
        }
        if (ORTE_JOB_STATE_RESTART == jdt->state) {
            /* reset the job parameters */
            orte_plm_base_reset_job(jdt);

            /* re-map the job */
            if (ORTE_SUCCESS != (rc = orte_rmaps.map_job(jdt))) {
                ORTE_ERROR_LOG(rc);
                continue;
            }         

            bfr = OBJ_NEW(opal_buffer_t);
            /* indicate the target DVM */
            jfam = ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid);
            opal_dss.pack(bfr, &jfam, 1, OPAL_UINT16);

            /* get the launch data */
            if (ORTE_SUCCESS != (rc = orte_odls.get_add_procs_data(bfr, jdt->jobid))) {
                ORTE_ERROR_LOG(rc);
                OBJ_RELEASE(bfr);
                return;
            }
            /* send it to the daemons */
            if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_SYS_CHANNEL,
                                                         NULL, ORCM_PNP_TAG_COMMAND,
                                                         NULL, 0, bfr, cbfunc, NULL))) {
                ORTE_ERROR_LOG(rc);
            }
        }
    }
}

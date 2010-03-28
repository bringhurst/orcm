/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config_private.h"
#include "include/constants.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rmcast/rmcast_types.h"

#include "mca/pnp/base/public.h"
#include "mca/pnp/base/private.h"

void orcm_pnp_base_push_data(orcm_pnp_source_t *src, opal_buffer_t *buf)
{
    src->msgs[src->end] = buf;
    /* move to next location, circling around if reqd */
    src->end = (1 + src->end) % ORCM_PNP_MAX_MSGS;
    return;
}

opal_buffer_t* orcm_pnp_base_pop_data(orcm_pnp_source_t *src)
{
    opal_buffer_t *buf;
    
    if (src->start == src->end) {
        /* no data available */
        return NULL;
    }
    
    /* save the location */
    buf = src->msgs[src->start];
    
    /* move to next location, circling around if reqd */
    src->start = (1 + src->start) % ORCM_PNP_MAX_MSGS;
}

#define HEARTBEAT_CK    2

static void heart_sent(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       opal_buffer_t *buf,
                       void *cbdata)
{
    OBJ_RELEASE(buf);
}

static void heartbeat(int fd, short flags, void *arg)
{
    opal_buffer_t *buf;
    orcm_pnp_tag_t tag = ORCM_PNP_HEARTBEAT;
    opal_event_t *tmp = (opal_event_t*)arg;
    struct timeval now;
    int rc;
    
    /* setup the buffer */
    buf = OBJ_NEW(opal_buffer_t);
    
    /* pass along any pending messages */
    OPAL_THREAD_LOCK(&orcm_pnp_heartbeat.lock);
    while (orcm_pnp_heartbeat.msg_pending) {
        opal_condition_wait(&orcm_pnp_heartbeat.cond, &orcm_pnp_heartbeat.lock);
    }
    opal_dss.copy_payload(buf, &orcm_pnp_heartbeat.msgs);
    OBJ_DESTRUCT(&orcm_pnp_heartbeat.msgs);
    OBJ_CONSTRUCT(&orcm_pnp_heartbeat.msgs, opal_buffer_t);
    orcm_pnp_heartbeat.msg_pending = false;
    opal_condition_broadcast(&orcm_pnp_heartbeat.cond);
    OPAL_THREAD_UNLOCK(&orcm_pnp_heartbeat.lock);
    
    /* send the heartbeat to the system channel */
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_buffer_nb(ORCM_PNP_SYS_CHANNEL, NULL,
                                                        ORCM_PNP_HEARTBEAT, buf,
                                                        heart_sent, NULL))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    
    /* reset the timer */
    now.tv_sec = orte_heartbeat_rate;
    now.tv_usec = 0;
    opal_evtimer_add(tmp, &now);
    
CLEANUP:
    OBJ_DESTRUCT(&buf);    
}

/* this function automatically gets periodically called
 * by the event library so we can check on the state
 * of the various orteds
 */
static void check_heartbeat(int fd, short dummy, void *arg)
{
    int v;
    orte_proc_t *proc;
    orte_job_t *daemons;
    struct timeval timeout;
    bool died = false;
    opal_event_t *tmp = (opal_event_t*)arg;
    struct timeval now;
    
    OPAL_OUTPUT_VERBOSE((1, orcm_pnp_base.output,
                         "%s pnp:base:check_heartbeat",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* if we are aborting or shutting down, ignore this */
    if (orte_abnormal_term_ordered || 0 == orte_heartbeat_rate) {
        return;
    }
    
    /* get the job object for the daemons */
    if (NULL == (daemons = orte_get_job_data_object(ORTE_PROC_MY_NAME->jobid))) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        return;
    }
    
    /* get current time */
    gettimeofday(&timeout, NULL);
    
    /* cycle through the daemons - make sure we check them all
     * in case multiple daemons died so all of those that did die
     * can be appropriately flagged
     */
    for (v=1; v < daemons->procs->size; v++) {
        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(daemons->procs, v))) {
            continue;
        }
        if ((timeout.tv_sec - proc->beat) > HEARTBEAT_CK*orte_heartbeat_rate) {
            /* declare this orted dead */
            proc->state = ORTE_PROC_STATE_ABORTED;
            proc->exit_code = ORTE_ERROR_DEFAULT_EXIT_CODE;
            if (NULL == daemons->aborted_proc) {
                daemons->aborted_proc = proc;
            }
            ORTE_UPDATE_EXIT_STATUS(ORTE_ERROR_DEFAULT_EXIT_CODE);
            died = true;
        }
    }
    
    /* if any daemon died, abort */
    if (died) {
        orte_plm_base_launch_failed(ORTE_PROC_MY_NAME->jobid, -1,
                                    ORTE_ERROR_DEFAULT_EXIT_CODE, ORTE_JOB_STATE_ABORTED);
        return;
    }
    
    /* reset the timer */
    now.tv_sec = HEARTBEAT_CK*orte_heartbeat_rate;
    now.tv_usec = 0;
    opal_evtimer_add(tmp, &now);
}

static void recv_heartbeat(int status,
                           orte_process_name_t *sender,
                           orcm_pnp_tag_t tag,
                           opal_buffer_t *buf,
                           void *cbdata)
{
    
}

void orcm_pnp_base_start_heart(char *app, char *version, char *release)
{
    /* if the heartbeat rate > 0, then start the heart */
    if (0 < orte_heartbeat_rate) {
        /* setup to issue our own heartbeats */
        ORTE_TIMER_EVENT(orte_heartbeat_rate, 0, check_heartbeat);
        /* setup to check for others */
        ORTE_TIMER_EVENT(HEARTBEAT_CK*orte_heartbeat_rate, 0, heartbeat);
        /* setup to recv other heartbeats */
        orcm_pnp.register_input_buffer(app, version, release,
                                       ORCM_PNP_SYS_CHANNEL,
                                       ORCM_PNP_HEARTBEAT,
                                       recv_heartbeat);
    }
}

void orcm_pnp_base_stop_heart(void)
{
    /* stop the heartbeat by simply turning the rate to 0 */
    orte_heartbeat_rate = 0;
}

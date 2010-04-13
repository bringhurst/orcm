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

static orte_job_t *daemons;

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
    int32_t rc=ORCM_SUCCESS;
    
    /* if we are aborting or shutting down, ignore this */
    if (orte_abnormal_term_ordered || 0 == orte_heartbeat_rate) {
        return;
    }
    
    /* setup the buffer */
    buf = OBJ_NEW(opal_buffer_t);
    
    opal_dss.pack(buf, &rc, 1, OPAL_INT32);
    
    /* send the heartbeat to the system channel */
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_buffer_nb(ORCM_PNP_SYS_CHANNEL, NULL,
                                                        ORCM_PNP_HEARTBEAT, buf,
                                                        heart_sent, NULL))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(buf);
        return;
    }
    
    /* reset the timer */
    now.tv_sec = orte_heartbeat_rate;
    now.tv_usec = 0;
    opal_evtimer_add(tmp, &now);
}

/* this function automatically gets periodically called
 * by the event library so we can check on the state
 * of the various orteds
 */
static void check_heartbeat(int fd, short dummy, void *arg)
{
    int v;
    orte_proc_t *proc;
    time_t timeout;
    opal_event_t *tmp = (opal_event_t*)arg;
    struct timeval now;
    
    OPAL_OUTPUT_VERBOSE((0, orcm_pnp_base.output,
                         "%s pnp:base:check_heartbeat",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* if we are aborting or shutting down, ignore this */
    if (orte_abnormal_term_ordered || 0 == orte_heartbeat_rate) {
        return;
    }
    
    /* get current time */
    timeout = time(NULL);
    
    /* cycle through the daemons - make sure we check them all
     * in case multiple daemons died so all of those that did die
     * can be appropriately flagged
     */
    for (v=0; v < daemons->procs->size; v++) {
        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(daemons->procs, v))) {
            continue;
        }
        if (proc->name.vpid == ORTE_PROC_MY_NAME->vpid) {
            /* don't check myself */
            continue;
        }
        if (ORTE_PROC_STATE_UNTERMINATED < proc->state) {
            /* already known dead */
            continue;
        }
        if (0 == proc->beat) {
            /* haven't heard from this proc yet */
            continue;
        }
        opal_output(0, "%s proc %s delta %d",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    ORTE_NAME_PRINT(&proc->name), (int)(timeout - proc->beat));
        if ((timeout - proc->beat) > HEARTBEAT_CK*orte_heartbeat_rate) {
            /* declare this proc dead */
            proc->state = ORTE_PROC_STATE_ABORTED;
            proc->exit_code = ORTE_ERROR_DEFAULT_EXIT_CODE;
            if (NULL == daemons->aborted_proc) {
                daemons->aborted_proc = proc;
            }
            /* inform the errmgr that we lost contact with someone so
             * it can decide what, if anything, to do about it
             */
            opal_output(0, "%s proc %s failed",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(&proc->name));
#if 0
            orte_errmgr.comm_failed(&proc->name, ORTE_ERROR_DEFAULT_EXIT_CODE);
#endif
        }
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
    int v;
    orte_proc_t *proc;

    /* if the heartbeat isn't from my job family, ignore it */
    if (ORTE_JOB_FAMILY(sender->jobid) != ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid)) {
        return;
    }
    
    opal_output(0, "%s GOT HEARTBEAT FROM %s",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                ORTE_NAME_PRINT(sender));

    /* find this proc */
    for (v=0; v < daemons->procs->size; v++) {
        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(daemons->procs, v))) {
            continue;
        }
        if (sender->vpid == proc->name.vpid) {
            /* found it - update heartbeat */
            proc->beat = time(NULL);
            return;
        }
    }
}

void orcm_pnp_base_start_heart(char *app, char *version, char *release)
{
    /* if the heartbeat rate > 0, then start the heart */
    if (0 < orte_heartbeat_rate) {
        /* get the job object for the daemons */
        if (NULL == (daemons = orte_get_job_data_object(0))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            return;
        }
        /* setup to issue our own heartbeats */
        ORTE_TIMER_EVENT(orte_heartbeat_rate, 0, heartbeat);
        /* setup to check for others */
        ORTE_TIMER_EVENT(HEARTBEAT_CK*orte_heartbeat_rate, 0, check_heartbeat);
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

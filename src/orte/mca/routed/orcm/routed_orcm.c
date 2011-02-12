/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"

#include <stddef.h>

#include "opal/threads/condition.h"
#include "opal/dss/dss.h"
#include "opal/class/opal_hash_table.h"
#include "opal/class/opal_bitmap.h"
#include "opal/util/bit_ops.h"
#include "opal/util/output.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/ess/ess.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/rml/rml_types.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"
#include "orte/runtime/orte_wait.h"
#include "orte/runtime/runtime.h"

#include "orte/mca/rml/base/rml_contact.h"

#include "orte/mca/routed/base/base.h"
#include "routed_orcm.h"

static int init(void);
static int finalize(void);
static int delete_route(orte_process_name_t *proc);
static int update_route(orte_process_name_t *target,
                        orte_process_name_t *route);
static orte_process_name_t get_route(orte_process_name_t *target);
static int init_routes(orte_jobid_t job, opal_buffer_t *ndat);
static int route_lost(const orte_process_name_t *route);
static bool route_is_defined(const orte_process_name_t *target);
static int update_routing_tree(void);
static orte_vpid_t get_routing_tree(opal_list_t *children);
static int get_wireup_info(opal_buffer_t *buf);
static int set_lifeline(orte_process_name_t *proc);
static size_t num_routes(void);

orte_routed_module_t orte_routed_orcm_module = {
    init,
    finalize,
    delete_route,
    update_route,
    get_route,
    init_routes,
    route_lost,
    route_is_defined,
    set_lifeline,
    update_routing_tree,
    get_routing_tree,
    get_wireup_info,
    num_routes,
    NULL
};

/* local globals */
static opal_condition_t         cond;
static opal_mutex_t             lock;
static orte_process_name_t      *lifeline=NULL;
static orte_process_name_t      local_lifeline;
static bool                     ack_recvd;


static int init(void)
{
    /* setup the global condition and lock */
    OBJ_CONSTRUCT(&cond, opal_condition_t);
    OBJ_CONSTRUCT(&lock, opal_mutex_t);

    lifeline = NULL;
    
    return ORTE_SUCCESS;
}

static int finalize(void)
{
    int rc;

    /* destruct the global condition and lock */
    OBJ_DESTRUCT(&cond);
    OBJ_DESTRUCT(&lock);

    lifeline = NULL;

    /* if I am an application process, indicate that I am
     * truly finalizing prior to departure
     */
    if (ORTE_PROC_IS_APP) {
        if (ORTE_SUCCESS != (rc = orte_routed_base_register_sync(false))) {
            ORTE_ERROR_LOG(rc);
            return rc;
        }
    }

    return ORTE_SUCCESS;
}

static int delete_route(orte_process_name_t *proc)
{
    int i;
    orte_routed_jobfam_t *jfam;
    uint16_t jfamily;
    
    if (proc->jobid == ORTE_JOBID_INVALID ||
        proc->vpid == ORTE_VPID_INVALID) {
        return ORTE_ERR_BAD_PARAM;
    }
    
    /* if I am an application, I don't have any routes
     * so there is nothing for me to do
     */
    if (ORTE_PROC_IS_APP) {
        return ORTE_SUCCESS;
    }
    
    OPAL_OUTPUT_VERBOSE((1, orte_routed_base_output,
                         "%s routed_orcm_delete_route for %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(proc)));
    
    
    /* remove any entries in the RML for this process */
    /*    orte_rml.purge(proc); */
    return ORTE_SUCCESS;
}

static int update_route(orte_process_name_t *target,
                        orte_process_name_t *route)
{ 
    /* if I am an application process, we don't update the route since
     * we automatically route everything through the local daemon. If I
     * am anything else, I route direct, so again don't need to update
     */
    return ORTE_SUCCESS;
}


static orte_process_name_t get_route(orte_process_name_t *target)
{
    orte_process_name_t *ret;
    orte_job_t *jdata;
    orte_proc_t *proc, *dproc;

    if (target->jobid == ORTE_JOBID_INVALID ||
        target->vpid == ORTE_VPID_INVALID) {
        ret = ORTE_NAME_INVALID;
        goto found;
    }
    
#if 0
    /* if it is me, then the route is just direct */
    if (OPAL_EQUAL == opal_dss.compare(ORTE_PROC_MY_NAME, target, ORTE_NAME)) {
        ret = target;
        goto found;
    }
    
    /* if I am a tool, always route direct */
    if (ORTE_PROC_IS_TOOL) {
        ret = target;
        goto found;
    }
    
    /* if I am an application process, always route via my local daemon */
    if (ORTE_PROC_IS_APP) {
        ret = ORTE_PROC_MY_DAEMON;
        goto found;
    }

    /* if the target is a different job family, then route direct */
    if (ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid) != ORTE_JOB_FAMILY(target->jobid)) {
        ret = target;
        goto found;
    }

    /* if I am a daemon or a scheduler, then see where this proc is located */
    if (ORTE_PROC_IS_DAEMON || ORTE_PROC_IS_SCHEDULER) {
        if (ORTE_PROC_MY_HNP->jobid == target->jobid &&
            ORTE_PROC_MY_HNP->vpid == target->vpid) {
            ret = target;
            goto found;
        }
        if (NULL == (jdata = orte_get_job_data_object(target->jobid))) {
            /* we are hosed */
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            ret = ORTE_NAME_INVALID;
            goto found;
        }   
        if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(jdata->procs, target->vpid))) {
            /* we are hosed */
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            ret = ORTE_NAME_INVALID;
            goto found;
        }
        dproc = proc->node->daemon;
        /* if the proc is one of my own, then route is direct */
        if (dproc->name.vpid == ORTE_PROC_MY_NAME->vpid) {
            ret = target;
        } else {
            ret = &dproc->name;
        }
        goto found;
    }

#endif
    /* for all other cases, route direct */
    ret = target;
    
 found:
    OPAL_OUTPUT_VERBOSE((2, orte_routed_base_output,
                         "%s routed_orcm_get(%s) --> %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(target), 
                         ORTE_NAME_PRINT(ret)));
    
    return *ret;
}

static int init_routes(orte_jobid_t job, opal_buffer_t *ndat)
{
    /* the orcm module routes all proc communications through
     * the local daemon. Daemons must identify which of their
     * daemon-peers is "hosting" the specified recipient and
     * route the message to that daemon. Daemon contact info
     * is handled elsewhere, so all we need to do here is
     * ensure that the procs are told to route through their
     * local daemon, and that daemons are told how to route
     * for each proc
     */
    int rc;
    orte_job_t *daemons;
    orte_proc_t *proc;

    /* if I am a tool, then see if I stand alone - otherwise,
     * setup the HNP info
     */
    if (ORTE_PROC_IS_TOOL) {
        OPAL_OUTPUT_VERBOSE((2, orte_routed_base_output,
                             "%s routed_orcm: init routes for TOOL job %s\n\thnp_uri %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_JOBID_PRINT(job),
                             (NULL == orte_process_info.my_hnp_uri) ? "NULL" : orte_process_info.my_hnp_uri));
        
        if (NULL == orte_process_info.my_hnp_uri) {
            return ORTE_SUCCESS;
        }
        
        /* set the contact info into the hash table */
        if (ORTE_SUCCESS != (rc = orte_rml.set_contact_info(orte_process_info.my_hnp_uri))) {
            ORTE_ERROR_LOG(rc);
            return(rc);
        }
        
        /* extract the hnp name and store it */
        if (ORTE_SUCCESS != (rc = orte_rml_base_parse_uris(orte_process_info.my_hnp_uri,
                                                           ORTE_PROC_MY_HNP, NULL))) {
            ORTE_ERROR_LOG(rc);
            return rc;
        }
        
        /* set our lifeline to the HNP - we will abort if that connection is lost */
        lifeline = ORTE_PROC_MY_HNP;
        
        return ORTE_SUCCESS;
    }
    
    /* if I am a daemon or HNP, then I have to extract the routing info for this job
     * from the data sent to me for launch and update the routing tables to
     * point at the daemon for each proc
     */
    if (ORTE_PROC_IS_DAEMON || ORTE_PROC_IS_SCHEDULER) {
        
        OPAL_OUTPUT_VERBOSE((2, orte_routed_base_output,
                             "%s routed_orcm: init routes for daemon job %s\n\thnp_uri %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_JOBID_PRINT(job),
                             (NULL == orte_process_info.my_hnp_uri) ? "NULL" : orte_process_info.my_hnp_uri));
        
        if (NULL == ndat) {
            /* indicates this is being called during orte_init.
             * Get the HNP's name for possible later use
             */
            if (NULL == orte_process_info.my_hnp_uri) {
                /* okay not to have one */
                return ORTE_SUCCESS;
            }
            /* set the contact info into the hash table */
            if (ORTE_SUCCESS != (rc = orte_rml.set_contact_info(orte_process_info.my_hnp_uri))) {
                ORTE_ERROR_LOG(rc);
                return(rc);
            }
            
            /* extract the hnp name and store it */
            if (ORTE_SUCCESS != (rc = orte_rml_base_parse_uris(orte_process_info.my_hnp_uri,
                                                               ORTE_PROC_MY_HNP, NULL))) {
                ORTE_ERROR_LOG(rc);
                return rc;
            }
            
            /* set our lifeline to the HNP - we will abort if that connection is lost */
            lifeline = ORTE_PROC_MY_HNP;
            
            /* create and store an orte_proc_t object for the HNP
             * so we can find it during startup
             */
            daemons = orte_get_job_data_object(ORTE_PROC_MY_HNP->jobid);
            if (NULL == (proc = (orte_proc_t*)opal_pointer_array_get_item(daemons->procs, ORTE_PROC_MY_HNP->vpid))) {
                /* new daemon - add it */
                proc = OBJ_NEW(orte_proc_t);
                proc->name.jobid = ORTE_PROC_MY_HNP->jobid;
                proc->name.vpid = ORTE_PROC_MY_HNP->vpid;
                proc->rml_uri = strdup(orte_process_info.my_hnp_uri);
                daemons->num_procs++;
                opal_pointer_array_set_item(daemons->procs, ORTE_PROC_MY_HNP->vpid, proc);
                /* update our num_procs */
                if (ORTE_PROC_IS_DAEMON) {
                    orte_process_info.num_procs++;
                }
            }
            /* daemons will send their contact info back to the HNP as
             * part of the message confirming they are read to go. HNP's
             * load their contact info during orte_init
             */
        }
        
        /* ignore any other call as we only talk to the HNP */
        
        OPAL_OUTPUT_VERBOSE((2, orte_routed_base_output,
                             "%s routed_orcm: completed init routes",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        
        return ORTE_SUCCESS;
    }
    
    
    if (ORTE_PROC_IS_HNP) {
        
        OPAL_OUTPUT_VERBOSE((2, orte_routed_base_output,
                             "%s routed_orcm: init routes for HNP job %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_JOBID_PRINT(job)));
        
        if (NULL == ndat) {
            /* the HNP has no lifeline */
            lifeline = NULL;
        } else {
            opal_output(0, "ROUTED CONTACT UPDATE");
        }

        return ORTE_SUCCESS;
    }

    {  /* MUST BE A PROC */
        /* if ndat != NULL, then this is an error as orcm does not support
         * such things
         */
        if (NULL != ndat) {            
            OPAL_OUTPUT_VERBOSE((1, orte_routed_base_output,
                                 "%s routed_orcm: init routes w/non-NULL data",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
            return ORTE_ERR_FATAL;
        }
        
        /* if ndat=NULL, then we are being called during orte_init. In this
         * case, we need to setup a few critical pieces of info
         */
        
        OPAL_OUTPUT_VERBOSE((1, orte_routed_base_output,
                             "%s routed_orcm: init routes for proc job %s\n\thnp_uri %s\n\tdaemon uri %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_JOBID_PRINT(job),
                             (NULL == orte_process_info.my_hnp_uri) ? "NULL" : orte_process_info.my_hnp_uri,
                             (NULL == orte_process_info.my_daemon_uri) ? "NULL" : orte_process_info.my_daemon_uri));
                
        if (NULL == orte_process_info.my_daemon_uri) {
            /* in this module, we absolutely MUST have this information - if
             * we didn't get it, then error out
             */
            opal_output(0, "%s ERROR: Failed to identify the local daemon's URI",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            opal_output(0, "%s ERROR: This is a fatal condition when the orcm router",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            opal_output(0, "%s ERROR: has been selected - either select the unity router",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            opal_output(0, "%s ERROR: or ensure that the local daemon info is provided",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            return ORTE_ERR_FATAL;
        }
            
        /* Set the contact info in the RML - this won't actually establish
         * the connection, but just tells the RML how to reach the daemon
         * if/when we attempt to send to it
         */
        if (ORTE_SUCCESS != (rc = orte_rml.set_contact_info(orte_process_info.my_daemon_uri))) {
            ORTE_ERROR_LOG(rc);
            return(rc);
        }
        /* we have to set the daemon's name */
        if (ORTE_SUCCESS != (rc = orte_rml_base_parse_uris(orte_process_info.my_daemon_uri,
                                                           ORTE_PROC_MY_DAEMON, NULL))) {
            ORTE_ERROR_LOG(rc);
            return rc;
        }
        
        /* Set the contact info in the RML - this won't actually establish
         * the connection, but just tells the RML how to reach the daemon
         * if/when we attempt to send to it
         */
        if (ORTE_SUCCESS != (rc = orte_rml.set_contact_info(orte_process_info.my_daemon_uri))) {
            ORTE_ERROR_LOG(rc);
            return(rc);
        }
        /* extract the daemon's name so we can update the routing table */
        if (ORTE_SUCCESS != (rc = orte_rml_base_parse_uris(orte_process_info.my_daemon_uri,
                                                           ORTE_PROC_MY_DAEMON, NULL))) {
            ORTE_ERROR_LOG(rc);
            return rc;
        }
        
        /* set our lifeline to the local daemon - we will abort if this connection is lost */
        lifeline = ORTE_PROC_MY_DAEMON;
        
        if (NULL == orte_process_info.my_hnp_uri) {
            /* okay not to have one */
            goto sync;
        }
        /* set the contact info into the hash table */
        if (ORTE_SUCCESS != (rc = orte_rml.set_contact_info(orte_process_info.my_hnp_uri))) {
            ORTE_ERROR_LOG(rc);
            return(rc);
        }
            
        /* extract the hnp name and store it */
        if (ORTE_SUCCESS != (rc = orte_rml_base_parse_uris(orte_process_info.my_hnp_uri,
                                                           ORTE_PROC_MY_HNP, NULL))) {
            ORTE_ERROR_LOG(rc);
            return rc;
        }

    sync:
        /* register ourselves -this sends a message to the daemon (warming up that connection)
         * and allows the daemon to get our contact info
         */
        if (ORTE_SUCCESS != (rc = orte_routed_base_register_sync(false))) {
            ORTE_ERROR_LOG(rc);
            return rc;
        }

        return ORTE_SUCCESS;
    }
}

static int route_lost(const orte_process_name_t *route)
{
    if (ORTE_PROC_IS_HNP) {
        /* take no further action */
        return ORTE_SUCCESS;
    }

    /* if we are not the HNP and lose the connection to the
     * lifeline, and we are NOT already,
     * in finalize, tell the OOB to abort.
     * NOTE: we cannot call abort from here as the OOB needs to first
     * release a thread-lock - otherwise, we will hang!!
     */
    if (!orte_finalizing &&
        NULL != lifeline &&
        OPAL_EQUAL == orte_util_compare_name_fields(ORTE_NS_CMP_ALL, route, lifeline)) {
        OPAL_OUTPUT_VERBOSE((2, orte_routed_base_output,
                             "%s routed:orcm: Connection to lifeline %s lost",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(lifeline)));
        return ORTE_ERR_FATAL;
    }

    /* we don't care about this one, so return success */
    return ORTE_SUCCESS;
}


static bool route_is_defined(const orte_process_name_t *target)
{
    return true;
}

static int set_lifeline(orte_process_name_t *proc)
{
    /* we have to copy the proc data because there is no
     * guarantee that it will be preserved
     */
    local_lifeline.jobid = proc->jobid;
    local_lifeline.vpid = proc->vpid;
    lifeline = &local_lifeline;
    
    return ORTE_SUCCESS;
}

static int update_routing_tree(void)
{
    /* nothing to do here */
    return ORTE_SUCCESS;
}

static orte_vpid_t get_routing_tree(opal_list_t *children)
{
    /* I have no parent or children */
    return ORTE_VPID_INVALID;
}

static int get_wireup_info(opal_buffer_t *buf)
{
    return ORTE_SUCCESS;
}

static size_t num_routes(void)
{
    return 0;
}

/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config.h"
#include "include/constants.h"

#include "opal/dss/dss.h"
#include "opal/util/output.h"

#include "orte/mca/rmcast/rmcast.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"

#include "mca/pnp/pnp.h"

#include "mca/leader/leader.h"
#include "mca/leader/base/private.h"
#include "mca/leader/fifl/leader_fifl.h"

/* API functions */

static int fifl_init(void);
static bool has_leader_failed(orcm_pnp_group_t *grp,
                              orcm_pnp_source_t *src,
                              orte_rmcast_seq_t seq_num);
static int set_leader(char *app,
                      char *version,
                      char *release,
                      int sibling,
                      orcm_leader_cbfunc_t cbfunc);
static int select_leader(orcm_pnp_group_t *grp);
static int fifl_finalize(void);

/* The module struct */

orcm_leader_base_module_t orcm_leader_fifl_module = {
    fifl_init,
    set_leader,
    select_leader,
    has_leader_failed,
    fifl_finalize
};

static int fifl_init(void)
{
    return ORCM_SUCCESS;
}
static int set_leader(char *app,
                      char *version,
                      char *release,
                      int sibling,
                      orcm_leader_cbfunc_t cbfunc)
{
    orcm_pnp_group_t *grp;
    opal_list_item_t *item;
    orcm_pnp_source_t *src;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                         "%s leader:fifl:set leader for triplet %s:%s:%s to sibling %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release,
                         sibling));

    /* get the grp corresponding to this app-triplet - the
     * function will create the grp if it doesn't already
     * exist
     */
    grp = orcm_pnp.get_group(app, version, release);
    
    /* flag that the leader for this group has been manually
     * set so we don't allow any automatic changes
     */
    grp->leader_set = true;
    
    /* set the callback function in case of leader failure */
    grp->leader_failed_cbfunc = cbfunc;
    
    /* if the specified sibling is ORCM_LEADER_WILDCARD, then
     * set the grp->leader to ORCM_SOURCE_WILDCARD
     */
    if (ORCM_LEADER_WILDCARD == sibling) {
        OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                             "%s leader:fifl:set leader to wildcard",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        grp->leader = ORCM_SOURCE_WILDCARD;
        return ORCM_SUCCESS;
    }
    
    /* search for the specified leader among the group's
     * known sources
     */
    for (item = opal_list_get_first(&grp->members);
         item != opal_list_get_end(&grp->members);
         item = opal_list_get_next(item)) {
        src = (orcm_pnp_source_t*)item;
        
        if (src->name.vpid == sibling) {
            grp->leader = src;
            OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                                 "%s leader:fifl:set leader to existing source %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(&(src->name))));
            return ORCM_SUCCESS;
        }
    }
    
    /* if we get here, then the source isn't yet
     * known - so create it
     */
    src = OBJ_NEW(orcm_pnp_source_t);
    src->name.jobid = ORTE_JOBID_WILDCARD; /* we don't know the jobid yet */
    src->name.vpid = sibling;
    opal_list_append(&grp->members, &src->super);
    grp->leader = src;
    OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                         "%s leader:fifl:set leader to new source %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(&(src->name))));
                                                    
    return ORCM_SUCCESS;
}

static int select_leader(orcm_pnp_group_t *grp)
{
    opal_list_item_t *item, *next;
    orcm_pnp_source_t *src;
    
    /* if the leader was manually set, then see if the
     * callback function was provided
     */
    if (grp->leader_set) {
        if (NULL != grp->leader_failed_cbfunc) {
            grp->leader_failed_cbfunc(grp->app, grp->version,
                                      grp->release, grp->leader->name.vpid);
            return ORCM_SUCCESS;
        }
        /* if the callback wasn't provided, then we switch
         * to auto-select
         */
        grp->leader_set = false;
    }
    
    /* take next "alive" source on list */
    item = opal_list_get_next((opal_list_item_t*)grp->leader);
    /* loop back around if at end */
    if (opal_list_get_end(&grp->members) == item) {
        item = opal_list_get_first(&grp->members);
    }
    while (item != (opal_list_item_t*)grp->leader) {
        src = (orcm_pnp_source_t*)item;
        if (!src->failed) {
            grp->leader = src;
            return ORCM_SUCCESS;
        }
        item = opal_list_get_next(item);
        /* loop back around if at end */
        if (item == opal_list_get_end(&grp->members)) {
            item = opal_list_get_first(&grp->members);
        }
    }
    /* nothing available */
    grp->leader = NULL;
    return ORCM_ERR_NOT_AVAILABLE;
}

static bool has_leader_failed(orcm_pnp_group_t *grp,
                              orcm_pnp_source_t *source,
                              orte_rmcast_seq_t seq_num)
{
    opal_list_item_t *item;
    orcm_pnp_source_t *src;
    int delta, maxval=-1, maxpkt=-1;
    bool failed=false;
    
    /* is the message out of sequence? */
    if (!orcm_leader_base_valid_sequence_number(source, seq_num)) {
        source->failed = true;
        if (source == grp->leader) {
            return true;
        }
        return false;
    }
        
    /* find the max difference in message indices */
    for (item = opal_list_get_first(&grp->members);
         item != opal_list_get_end(&grp->members);
         item = opal_list_get_next(item)) {
        src = (orcm_pnp_source_t*)item;
        /* ignore failed sources */
        if (src->failed) {
            continue;
        }
        /* find the max packet number */
        if (maxpkt < src->last_msg_num) {
            maxpkt = src->last_msg_num;
        }
        /* compute the max delta between the leader and all others */
        if (src == grp->leader) {
            /* ignore the current leader */
            continue;
        }
        delta = src->last_msg_num - grp->leader->last_msg_num;
        if (delta < 0) {
            /* leader is ahead - ignore */
            continue;
        }
        if (maxval < delta) {
            maxval = delta;
        }
    }
    
    /* is the difference larger than the trigger? */
    if (mca_leader_fifl_component.trigger < maxval) {
        /* mark the leader as having failed */
        grp->leader->failed = true;
        return true;
    }
    
    /* otherwise, the leader has not failed */
    return false;
}

static int fifl_finalize(void)
{
    return ORCM_SUCCESS;
}

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

#include "mca/leader/leader.h"
#include "mca/leader/base/private.h"
#include "mca/leader/fifl/leader_fifl.h"

/* API functions */

static int fifl_init(void);
static bool has_leader_failed(orcm_pnp_group_t *grp);
static int fifl_set(orcm_pnp_group_t *grp, orte_process_name_t *ldr);
static int fifl_finalize(void);

/* The module struct */

orcm_leader_base_module_t orcm_leader_fifl_module = {
    fifl_init,
    fifl_set,
    has_leader_failed,
    fifl_finalize
};

static int fifl_init(void)
{
    return ORCM_SUCCESS;
}

static int fifl_set(orcm_pnp_group_t *grp, orte_process_name_t *ldr)
{
    opal_list_item_t *item, *next;
    orcm_pnp_source_t *src, *leader=NULL;
    
    /* if the provided leader is NULL, then we are to select it ourselves */
    if (NULL == ldr) {
        /* take next "alive" source on list */
        item = opal_list_get_next((opal_list_item_t*)grp->leader);
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
            if (item == opal_list_get_end(&grp->members)) {
                item = opal_list_get_first(&grp->members);
            }
        }
        /* nothing available */
        return ORCM_ERR_NOT_AVAILABLE;
    }
    
    /* if the provided leader is wildcard, then allow everyone to flow through */
    if (ORTE_JOBID_WILDCARD == ldr->jobid && ORTE_VPID_WILDCARD == ldr->vpid)
    {
        grp->leader = NULL;
        return ORCM_SUCCESS;
    }
    
    /* if the leader has been specified, flag it in our list of sources */
    for (item = opal_list_get_first(&grp->members);
         item != opal_list_get_end(&grp->members);
         item = opal_list_get_next(item)) {
        src = (orcm_pnp_source_t*)item;
        if (src->name.jobid == ldr->jobid && src->name.vpid == ldr->vpid) {
            /* is this source "alive"? */
            if (src->failed) {
                return ORCM_ERR_NOT_AVAILABLE;
            }
            /* assign it */
            grp->leader = src;
            return ORCM_SUCCESS;
        }
    }
    
    /* if none of those conditions were met, then something is wrong */
    return ORCM_ERR_NOT_FOUND;
}

static bool has_leader_failed(orcm_pnp_group_t *grp)
{
    opal_list_item_t *item;
    orcm_pnp_source_t *src;
    int delta, maxval=-1, maxpkt=-1;
    bool failed;
    
    /* find the max difference in message indices */
    for (item = opal_list_get_first(&grp->members);
         item != opal_list_get_end(&grp->members);
         item = opal_list_get_next(item)) {
        src = (orcm_pnp_source_t*)item;
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
    
    /* if the leader of this grp is NULL, then anyone can
     * provide a message. Any sources that have fallen more than
     * the specified limit behind the maximum one will be declared
     * "failed" and ignored from now on
     */
    if (NULL == grp->leader) {
        failed = false;
        for (item = opal_list_get_first(&grp->members);
             item != opal_list_get_end(&grp->members);
             item = opal_list_get_next(item)) {
            src = (orcm_pnp_source_t*)item;
            delta = maxpkt - src->last_msg_num;
            if (mca_leader_fifl_component.trigger < delta) {
                src->failed = true;
                failed = true;
            }
        }
        return failed;
    }
    
    /* is the difference larger than the trigger? */
    if (mca_leader_fifl_component.trigger < maxval) {
        return true;
    }
    
    /* otherwise, the leader has not failed */
    return false;
}

static int fifl_finalize(void)
{
    return ORCM_SUCCESS;
}

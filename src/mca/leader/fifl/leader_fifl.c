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

#include "opal/dss/dss.h"
#include "opal/util/output.h"
#include "opal/threads/mutex.h"

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
                              orcm_pnp_source_t *src);
static int set_leader(char *app,
                      char *version,
                      char *release,
                      orte_vpid_t sibling,
                      orcm_leader_cbfunc_t cbfunc);
static int select_leader(orcm_pnp_group_t *grp);
static orcm_pnp_source_t* get_leader(orcm_pnp_group_t *grp);
static int fifl_finalize(void);

/* The module struct */

orcm_leader_base_module_t orcm_leader_fifl_module = {
    fifl_init,
    set_leader,
    select_leader,
    has_leader_failed,
    get_leader,
    fifl_finalize
};

/* list of specified leaders */
static opal_list_t leaders;

/* local thread support */
static opal_mutex_t lock;

/* local functions */
static orcm_leader_t* find_leader(orcm_pnp_group_t *grp);

static int fifl_init(void)
{
    /* setup the threading support */
    OBJ_CONSTRUCT(&lock, opal_mutex_t);

    /* setup the list of leaders */
    OBJ_CONSTRUCT(&leaders, opal_list_t);
    
    return ORCM_SUCCESS;
}
static int set_leader(char *app,
                      char *version,
                      char *release,
                      orte_vpid_t sibling,
                      orcm_leader_cbfunc_t cbfunc)
{
    orcm_pnp_group_t *grp;
    opal_list_item_t *item;
    orcm_pnp_source_t *src;
    orcm_leader_t *leader, *ldr;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                         "%s leader:fifl:set leader for triplet %s:%s:%s to sibling %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release,
                         ORTE_VPID_PRINT(sibling)));

    OPAL_THREAD_LOCK(&lock);
    
    /* see if we already have this leader on our list */
    leader = NULL;
    for (item = opal_list_get_first(&leaders);
         item != opal_list_get_end(&leaders);
         item = opal_list_get_next(item)) {
        ldr = (orcm_leader_t*)item;
        if (0 != strcasecmp(app, ldr->app)) {
            continue;
        }
        if ((NULL == version && NULL != ldr->version) ||
            (NULL != version && NULL == ldr->version)) {
            continue;
        }
        /* either both are NULL or both are non-NULL */
        if (NULL != version && 0 != strcasecmp(version, ldr->version)) {
            continue;
        }
        if ((NULL == release && NULL != ldr->release) ||
            (NULL != release && NULL == ldr->release)) {
            continue;
        }
        /* either both are NULL or both are non-NULL */
        if (NULL != release && 0 != strcasecmp(release, ldr->release)) {
            continue;
        }
        /* have a match */
        leader = ldr;
        break;
    }
    if (NULL == leader) {
        /* new entry */
        leader = OBJ_NEW(orcm_leader_t);
        leader->app = strdup(app);
        if (NULL != version) {
            leader->version = strdup(version);
        }
        if (NULL != release) {
            leader->release = strdup(release);
        }
        leader->lead_rank = sibling;
        leader->cbfunc = cbfunc;
        opal_list_append(&leaders, &leader->super);
    }
    OPAL_THREAD_UNLOCK(&lock);
    
    return ORCM_SUCCESS;
}

static int select_leader(orcm_pnp_group_t *grp)
{
    orcm_pnp_source_t *src;
    orcm_leader_t *ldr;
    int i;
    
    OPAL_THREAD_LOCK(&lock);
    
    /* find the leader object for this grp */
    if (NULL == (ldr = find_leader(grp))) {
        /* don't have one - nothing to do */
        OPAL_THREAD_UNLOCK(&lock);
        return ORCM_SUCCESS;
    }
    
    /* we do have a leader object for this grp. If the
     * leader was specified as WILDCARD, then we leave
     * it that way
     */
    if (ORCM_LEADER_WILDCARD == ldr->lead_rank) {
        OPAL_THREAD_UNLOCK(&lock);
        return ORCM_SUCCESS;
    }
    
    /* if the leader is invalid, take the first alive source */
    if (ORTE_VPID_INVALID == ldr->lead_rank) {
        ldr->lead_rank = 0;
    }
    
    /* take the next alive source in the array */
    for (i=ldr->lead_rank; i < grp->members.size; i++) {
        if (NULL == (src = (orcm_pnp_source_t*)opal_pointer_array_get_item(&grp->members, i))) {
            continue;
        }
        /* take first alive source */
        if (!src->failed) {
            ldr->lead_rank = i;
            OPAL_THREAD_UNLOCK(&lock);
            return ORCM_SUCCESS;
        }
    }
    
    /* if we didn't find one above, try below */
    for (i=0; i < ldr->lead_rank; i++) {
        if (NULL == (src = (orcm_pnp_source_t*)opal_pointer_array_get_item(&grp->members, i))) {
            continue;
        }
        /* take first alive source */
        if (!src->failed) {
            ldr->lead_rank = i;
            OPAL_THREAD_UNLOCK(&lock);
            return ORCM_SUCCESS;
        }
    }
    
    /* if we couldn't find anything, let the caller know */
    ldr->lead_rank = ORTE_VPID_INVALID;
    OPAL_THREAD_UNLOCK(&lock);
    return ORCM_ERR_NOT_AVAILABLE;
}

static bool has_leader_failed(orcm_pnp_group_t *grp,
                              orcm_pnp_source_t *leader)
{
    orcm_pnp_source_t *src;
    int i, delta, maxval=-1, maxpkt=-1;
    
    OPAL_THREAD_LOCK(&lock);
    
    /* find the max difference in message indices */
    for (i=0; i < grp->members.size; i++) {
        if (NULL == (src = (orcm_pnp_source_t*)opal_pointer_array_get_item(&grp->members, i))) {
            continue;
        }
        /* ignore failed sources */
        if (src->failed) {
            continue;
        }
        /* find the max packet number */
        if (maxpkt < src->last_msg_num) {
            maxpkt = src->last_msg_num;
        }
        /* compute the max delta between the leader and all others */
        if (src == leader) {
            /* ignore the current leader */
            continue;
        }
        delta = src->last_msg_num - leader->last_msg_num;
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
        leader->failed = true;
        OPAL_THREAD_UNLOCK(&lock);
        return true;
    }
    
    /* otherwise, the leader has not failed */
    OPAL_THREAD_UNLOCK(&lock);
    return false;
}

static orcm_pnp_source_t* get_leader(orcm_pnp_group_t *grp)
{
    orcm_leader_t *ldr;
    orcm_pnp_source_t *src;
    
    OPAL_THREAD_LOCK(&lock);
    
    /* find the leader object for this grp */
    if (NULL == (ldr = find_leader(grp))) {
        /* don't have one - assume wildcard */
        OPAL_THREAD_UNLOCK(&lock);
        return NULL;
    }
    
    if (ORCM_LEADER_WILDCARD == ldr->lead_rank) {
        OPAL_THREAD_UNLOCK(&lock);
        return NULL;
    }
    
    src = opal_pointer_array_get_item(&grp->members, ldr->lead_rank);
    OPAL_THREAD_UNLOCK(&lock);
    return src;
}

static int fifl_finalize(void)
{
    opal_list_item_t *item;
    
    while (NULL != (item = opal_list_remove_first(&leaders))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&leaders);
    OBJ_DESTRUCT(&lock);
    
    return ORCM_SUCCESS;
}


/****   LOCAL FUNCTIONS    ****/
static orcm_leader_t* find_leader(orcm_pnp_group_t *grp)
{
    opal_list_item_t *item;
    orcm_leader_t *ldr;
    
    /* if the group version and/or release are NULL, then
     * this group cannot have a leader
     */
    if (NULL == grp->version || NULL == grp->release) {
        return NULL;
    }
    
    for (item = opal_list_get_first(&leaders);
         item != opal_list_get_end(&leaders);
         item = opal_list_get_next(item)) {
        ldr = (orcm_leader_t*)item;
        if (0 != strcasecmp(grp->app, ldr->app)) {
            continue;
        }
        if (NULL != ldr->version && 0 != strcasecmp(ldr->version, grp->version)) {
            continue;
        }
        if (NULL != ldr->release && 0 != strcasecmp(ldr->release, grp->release)) {
            continue;
        }
        /* matches */
        return ldr;
    }

    return NULL;
}

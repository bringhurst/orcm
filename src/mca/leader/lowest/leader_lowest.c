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

#include "opal/class/opal_list.h"
#include "opal/util/output.h"
#include "opal/threads/threads.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"

#include "mca/pnp/pnp.h"
#include "runtime/orcm_globals.h"
#include "util/triplets.h"

#include "mca/leader/leader.h"
#include "mca/leader/base/private.h"
#include "mca/leader/lowest/leader_lowest.h"

/* API functions */

static int lowest_init(void);
static void lowest_finalize(void);
static bool deliver_msg(const char *stringid, const orte_process_name_t *src);
static int set_leader(const char *app,
                      const char *version,
                      const char *release,
                      const orte_process_name_t *sibling,
                      orcm_notify_t notify,
                      orcm_leader_cbfunc_t cbfunc);
static int get_leader(const char *app, const char *version,
                      const char *release, orte_process_name_t *leader);
static void proc_failed(const char *stringid, const orte_process_name_t *failed);

/* The module struct */

orcm_leader_base_module_t orcm_leader_lowest_module = {
    lowest_init,
    lowest_finalize,
    deliver_msg,
    set_leader,
    get_leader,
    proc_failed
};

/* local globals */
static opal_mutex_t lock;
static opal_condition_t cond;
static bool active;

static int lowest_init(void)
{
    int ret;

    OBJ_CONSTRUCT(&lock, opal_mutex_t);
    OBJ_CONSTRUCT(&cond, opal_condition_t);
    active = false;

    return ORCM_SUCCESS;
}

static void lowest_finalize(void)
{
    opal_list_item_t *item;

    /* ensure any outstanding requests are complete */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    OPAL_THREAD_UNLOCK(&lock);

    OBJ_DESTRUCT(&lock);
    OBJ_DESTRUCT(&cond);

}

static orcm_source_t* lowest_vpid_alive(orcm_triplet_group_t *grp)
{
    orte_vpid_t vpid;
    orcm_source_t *src;

    /* cycle thru the grp members and find the lowest alive vpid, if any */
    for (vpid=0; vpid < grp->members.size; vpid++) {
        if (NULL == (src = (orcm_source_t*)opal_pointer_array_get_item(&grp->members, vpid))) {
            continue;
        }
        if (src->alive) {
            /* found what you wanted - lock the source and return it */
            OPAL_ACQUIRE_THREAD(&src->lock, &src->cond, &src->in_use);
            return src;
        }
        /* be sure to release the source thread! */
        OPAL_RELEASE_THREAD(&src->lock, &src->cond, &src->in_use);
    }

    /* get here if not found */
    return NULL;
}

static int set_leader(const char *app,
                      const char *version,
                      const char *release,
                      const orte_process_name_t *leader,
                      orcm_notify_t notify,
                      orcm_leader_cbfunc_t cbfunc)
{
    orcm_triplet_t *trp;
    orcm_triplet_group_t *grp;
    orcm_source_t *src;
    int j;

    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);

    OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                         "%s leader:lowest:set_leader for %s %s %s to %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release,
                         (NULL == leader) ? "NULL" : ORTE_NAME_PRINT(leader)));

    /* find this triplet - create it if not found */
    trp = orcm_get_triplet(app, version, release, true);

    /* if the leader is NULL, then this is being called for the purpose
     * of defining callback policy => cbfunc must be provided
     */
    if (NULL == leader) {
        if (NULL == cbfunc) {
            opal_output(0, "%s SET LEADER CALLED WITHOUT CBFUNC",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            return ORTE_ERR_BAD_PARAM;
        }
        /* set the cbfunc and policy */
        trp->notify = notify;
        trp->leader_cbfunc = cbfunc;
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORCM_SUCCESS;
    }

    /* record the leadership policy */
    trp->leader_policy.jobid = leader->jobid;
    trp->leader_policy.vpid = leader->vpid;

    /* set the leader for this triplet */

    /* if the jobid is WILDCARD, then we consider all members of the
     * triplet, regardless of their jobid
     */
    if (ORTE_JOBID_WILDCARD == leader->jobid) {
        /* indicate that all groups are valid */
        trp->leader.jobid = ORTE_JOBID_WILDCARD;
        /* indicate the intended selection, to be done according
         * to the following rules:
         *
         * ORTE_VPID_WILDCARD => pass thru all messages
         * ORTE_VPID_INVALID => pass thru messages only from
         *                      the lowest vpid in each group
         * specific value => pass thru messages only from the
         *                   specified vpid of each group
         */
        trp->leader.vpid = leader->vpid;
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORCM_SUCCESS;
    }

    /* if the jobid is given as INVALID, then we only consider members
     * of the grp who shares our jobid. Otherwise, the jobid was a
     * specific value, so we only consider members from that grp
     */
    if (ORTE_JOBID_INVALID == leader->jobid) {
        /* get the triplet group with my jobid - create it if missing */
        grp = orcm_get_triplet_group(trp, ORTE_PROC_MY_NAME->jobid, true);
        /* indicate that only members of this group are to be passed thru */
        trp->leader.jobid = ORTE_PROC_MY_NAME->jobid;
    } else {
        /* get the triplet group from the specified jobid - create it if missing */
        grp = orcm_get_triplet_group(trp, leader->jobid, true);
        trp->leader.jobid = leader->jobid;
    }

    /* check the specified vpid */
    if (ORTE_VPID_WILDCARD == leader->vpid) {
        /* pass thru all messages from this group */
        trp->leader.vpid = ORTE_VPID_WILDCARD;
    } else if (ORTE_VPID_INVALID == leader->vpid) {
        /* get the lowest vpid alive from within that grp */
        if (NULL != (src = lowest_vpid_alive(grp))) {
            /* this is the leader */
            OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                                 "%s leader:lowest: leader for %s %s %s set to %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 (NULL == app) ? "NULL" : app,
                                 (NULL == version) ? "NULL" : version,
                                 (NULL == release) ? "NULL" : release,
                                 ORTE_NAME_PRINT(&src->name)));
            trp->leader.jobid = src->name.jobid;
            trp->leader.vpid = src->name.vpid;
            OPAL_RELEASE_THREAD(&src->lock, &src->cond, &src->in_use);
            goto release;
        }
        /* if one wasn't found - e.g., we may not have heard from anyone yet - then
         * don't worry about it. Indicate that something should be done once messages
         * start to arrive
         */
        trp->leader.vpid = ORTE_VPID_INVALID;
    } else {
        /* just set the leader to the specified value - note that
         * no messages will be delivered until the specified
         * process becomes available
         */
        trp->leader.vpid = leader->vpid;
    }

 release:
    /* release the triplet */
    OPAL_RELEASE_THREAD(&trp->lock, &trp->cond, &trp->in_use);

    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    return ORCM_SUCCESS;
}

static bool deliver_msg(const char *stringid, const orte_process_name_t *src)
{
    bool ret;
    orcm_triplet_t *trp;
    int i;

    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);

    /* find this triplet - don't create it if not found */
    if (NULL == (trp = orcm_get_triplet_stringid(stringid))) {
        /* unknown */
        OPAL_OUTPUT_VERBOSE((0, orcm_leader_base.output,
                             "%s leader: stringid %s is unknown - can't deliver msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), stringid));
        ret = false;
    } else if (ORTE_JOBID_WILDCARD == trp->leader.jobid &&
               ORTE_VPID_WILDCARD == trp->leader.vpid) {
        OPAL_OUTPUT_VERBOSE((0, orcm_leader_base.output,
                             "%s leader: stringid %s is wildcard leader - deliver msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), stringid));
        /* release the triplet */
        OPAL_RELEASE_THREAD(&trp->lock, &trp->cond, &trp->in_use);
        ret = true;
    } else {
        if (trp->leader.jobid == src->jobid &&
            trp->leader.vpid == src->vpid) {
            ret = true;
        } else {
            ret = false;
        }
        /* release the triplet */
        OPAL_RELEASE_THREAD(&trp->lock, &trp->cond, &trp->in_use);
    }

    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    return ret;
}

static int get_leader(const char *app, const char *version,
                      const char *release, orte_process_name_t *leader)
{
    orcm_triplet_t *trp;

    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);

    /* find this triplet - don't create it if not found */
    if (NULL == (trp = orcm_get_triplet(app, version, release, false))) {
        /* unknown */
        leader->jobid = ORTE_JOBID_WILDCARD;
        leader->vpid = ORTE_VPID_WILDCARD;
    } else {
        leader->jobid = trp->leader.jobid;
        leader->vpid = trp->leader.vpid;
        /* release the triplet */
        OPAL_RELEASE_THREAD(&trp->lock, &trp->cond, &trp->in_use);
    }

 cleanup:
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    return ORCM_SUCCESS;
}

static void proc_failed(const char *stringid, const orte_process_name_t *failed)
{
    orcm_triplet_t *trp;
    orcm_triplet_group_t *grp;
    orte_process_name_t old_leader;

    /* get the triplet corresponding to this process */
    if (NULL == (trp = orcm_get_triplet_stringid(stringid))) {
        /* unknown triplet */
        return;
    }
    /* get the group containing this process */
    grp = orcm_get_triplet_group(trp, failed->jobid, true);
    
    /* do we need a new leader? */
    if (ORTE_JOBID_WILDCARD == trp->leader_policy.jobid) {
        /* we know that all groups are valid sources to provide
         * leaders - see which method we are to use
         */
        if (ORTE_VPID_WILDCARD == trp->leader_policy.vpid) {
            /* all messages pass thru - there is no real leader
             * and hence no selection need be done
             */
        } else if (ORTE_VPID_INVALID == trp->leader_policy.vpid) {
            /* pass messages only from the lowest vpid in each
             * group, so see if this was the one for its group
             */
            if (grp->leader == failed->vpid) {
                /* save the old leader */
                old_leader.jobid = failed->jobid;
                old_leader.vpid = failed->vpid;
                /* get a new one */
            }
        } else {
            /* a specific value was given - see if this was the one */
            if (trp->leader.vpid == failed->vpid) {
                /* record the old leader */
                old_leader.jobid = failed->jobid;
                old_leader.vpid = failed->vpid;
                /* we don't replace the leader in this case */
            }
        }
    } else if (ORTE_JOBID_INVALID == trp->leader_policy.vpid) {
    }

    /* do we need to notify of this failure? */
    if (ORCM_NOTIFY_ANY == trp->notify) {
        /* we retain the thread lock on the triplet as the
         * callback function is NOT allowed to modify it
         */
        trp->leader_cbfunc(stringid, failed, &trp->leader);
    } else if (ORCM_NOTIFY_LDR == trp->notify) {
    }

}


#if 0
        /* see if this proc was a leader */
        if (ORTE_JOBID_WILDCARD == trp->leader.jobid ||
            failed->jobid == trp->leader.jobid) {
            /* if the vpid is wildcard or the specific value, then notify */
            if (ORTE_VPID_WILDCARD == trp->leader.vpid ||
                failed->vpid == trp->leader.vpid) {
                trp->leader_cbfunc(stringid, failed, &trp->leader);
            } else if (ORTE_VPID_INVALID == trp->leader.vpid) {
                /* the vpid must be invalid, which means the lowest vpid
                 * in each group is a leader - check this proc's group
                 * to see if it is the one
                 */
                grp = orcm_get_triplet_group(trp, failed->jobid, true);
                if (grp->leader == failed->vpid) {
                    /* find the replacement leader */
                    if (NULL == (src = lowest_vpid_alive(grp))) {
                        trp->leader_cbfunc(stringid, failed, NULL);
                    } else {
                        /* mark the new leader */
                        grp->leader = src->name.vpid;
                        trp->leader_cbfunc(stringid, failed, &src->name);
                        OPAL_RELEASE_THREAD(&src->lock, &src->cond, &src->in_use);
                    }
                }
            }

#endif

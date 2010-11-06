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
#include "mca/leader/base/public.h"
#include "mca/leader/lowest/leader_lowest.h"

/* API functions */

static int lowest_init(void);
static void lowest_finalize(void);
static bool deliver_msg(const char *stringid,
                        const orte_process_name_t *src,
                        const orte_rmcast_channel_t channel,
                        const orte_rmcast_seq_t seq_num);
static int set_policy(const char *app,
                      const char *version,
                      const char *release,
                      const orte_process_name_t *policy,
                      orcm_notify_t notify,
                      orcm_leader_cbfunc_t cbfunc);
static int set_leader(const char *app,
                      const char *version,
                      const char *release,
                      const orte_process_name_t *leader);
static int get_leader(const char *app, const char *version,
                      const char *release, orte_process_name_t *leader);
static void proc_failed(const char *stringid, const orte_process_name_t *failed);

/* The module struct */

orcm_leader_base_module_t orcm_leader_lowest_module = {
    lowest_init,
    lowest_finalize,
    set_policy,
    deliver_msg,
    set_leader,
    get_leader,
    proc_failed
};

/* local globals */
static opal_mutex_t lock;
static opal_condition_t cond;
static bool active;

static orcm_source_t* lowest_vpid_alive(orcm_triplet_group_t *grp);

static int lowest_init(void)
{
    int ret;

    /* construct the local thread protection */
    OBJ_CONSTRUCT(&lock, opal_mutex_t);
    OBJ_CONSTRUCT(&cond, opal_condition_t);
    active = false;

    /* define the default leader policy */
    orcm_default_leader_policy.jobid = ORTE_JOBID_WILDCARD;
    orcm_default_leader_policy.vpid = ORTE_VPID_INVALID;

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

static void eval_policy(orcm_triplet_t *trp)
{
    orcm_triplet_group_t *grp;
    orcm_source_t *src;
    orte_process_name_t *policy;
    int i, j;

    /* shorthand */
    policy = &trp->leader_policy;

    /* if the triplet is an "orcmd", then we must
     * retain a leadership of wildcard as any orcmd could send
     * to us
     */
    if (orcm_triplet_cmp(trp->string_id, "orcmd:@:@")) {
        OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                             "%s leader:lowest: leader for %s set to %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             trp->string_id, ORTE_NAME_PRINT(ORTE_NAME_WILDCARD)));
        trp->leader.jobid = ORTE_JOBID_WILDCARD;
        trp->leader.vpid = ORTE_VPID_WILDCARD;
        trp->leader_set = true;
        return;
    }

    /* if the policy jobid is WILDCARD, then we consider all members of the
     * triplet, regardless of their jobid
     */
    if (ORTE_JOBID_WILDCARD == policy->jobid) {
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
        trp->leader.vpid = policy->vpid;
        /* see if we can set a leader */
        if (ORTE_VPID_INVALID == policy->vpid) {
            /* find the lowest known group */
            for (i=0; i < trp->groups.size; i++) {
                if (NULL == (grp = (orcm_triplet_group_t*)opal_pointer_array_get_item(&trp->groups, i))) {
                    continue;
                }
                /* are any of its known procs alive? */
                for (j=0; j < grp->members.size; j++) {
                    if (NULL == (src = (orcm_source_t*)opal_pointer_array_get_item(&grp->members, j))) {
                        continue;
                    }
                    OPAL_ACQUIRE_THREAD(&src->lock, &src->cond, &src->in_use);
                    if (src->alive) {
                        /* we have our winner! */
                        OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                                             "%s leader:lowest: leader for %s set to %s",
                                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                             trp->string_id, ORTE_NAME_PRINT(&src->name)));
                        trp->leader.jobid = src->name.jobid;
                        trp->leader.vpid = src->name.vpid;
                        trp->leader_set = true;
                        OPAL_RELEASE_THREAD(&src->lock, &src->cond, &src->in_use);
                        return;
                    }
                    OPAL_RELEASE_THREAD(&src->lock, &src->cond, &src->in_use);
                }
            }
        } else {
            /* set the flag accordingly */
            trp->leader_set = true;
        }
        return;
    }

    /* if the jobid is given as INVALID, then we only consider members
     * of the grp who shares our jobid. Otherwise, the jobid was a
     * specific value, so we only consider members from that grp
     */
    if (ORTE_JOBID_INVALID == policy->jobid) {
        /* get the triplet group with my jobid - create it if missing */
        grp = orcm_get_triplet_group(trp, ORTE_PROC_MY_NAME->jobid, true);
        /* indicate that only members of this group are to be passed thru */
        trp->leader.jobid = ORTE_PROC_MY_NAME->jobid;
    } else {
        /* get the triplet group from the specified jobid - create it if missing */
        grp = orcm_get_triplet_group(trp, policy->jobid, true);
        trp->leader.jobid = policy->jobid;
    }

    /* check the specified vpid */
    if (ORTE_VPID_WILDCARD == policy->vpid) {
        /* pass thru all messages from this group */
        trp->leader.vpid = ORTE_VPID_WILDCARD;
        /* set the flag */
        trp->leader_set = true;
    } else if (ORTE_VPID_INVALID == policy->vpid) {
        /* get the lowest vpid alive from within that grp */
        if (NULL != (src = lowest_vpid_alive(grp))) {
            /* this is the leader */
            OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                                 "%s leader:lowest: leader for %s set to %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 trp->string_id, ORTE_NAME_PRINT(&src->name)));
            trp->leader.jobid = src->name.jobid;
            trp->leader.vpid = src->name.vpid;
            /* set the flag */
            trp->leader_set = true;
            OPAL_RELEASE_THREAD(&src->lock, &src->cond, &src->in_use);
            return;
        }
        /* if one wasn't found - e.g., we may not have heard from anyone yet - then
         * don't worry about it. Indicate that something should be done once messages
         * start to arrive and leave the flag unset
         */
        trp->leader.vpid = ORTE_VPID_INVALID;
    } else {
        /* just set the leader to the specified value - note that
         * no messages will be delivered until the specified
         * process becomes available
         */
        trp->leader.vpid = policy->vpid;
        /* set the flag */
        trp->leader_set = true;
    }
}


static int set_policy(const char *app,
                      const char *version,
                      const char *release,
                      const orte_process_name_t *policy,
                      orcm_notify_t notify,
                      orcm_leader_cbfunc_t cbfunc)
{
    orcm_triplet_t *trp;
    int j;
    int rc=ORCM_SUCCESS;

    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);

    OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                         "%s leader:lowest:set_leader for %s %s %s to %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release,
                         (NULL == policy) ? "NULL" : ORTE_NAME_PRINT(policy)));

    /* find this triplet - create it if not found */
    trp = orcm_get_triplet(app, version, release, true);

    /* if the leader is NULL, then this is being called for the purpose
     * of defining callback policy => cbfunc must be provided
     */
    if (NULL == policy) {
        if (NULL == cbfunc) {
            opal_output(0, "%s SET LEADER CALLED WITHOUT CBFUNC",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            rc = ORTE_ERR_BAD_PARAM;
            goto release;
        }
        /* set the cbfunc and policy */
        trp->notify = notify;
        trp->leader_cbfunc = cbfunc;
        goto release;
    }

    /* record the leadership policy */
    trp->leader_policy.jobid = policy->jobid;
    trp->leader_policy.vpid = policy->vpid;

    /* record the notify policy */
    trp->notify = notify;
    trp->leader_cbfunc = cbfunc;

    /* set the leader for this triplet if we can - at the
     * least, set the fields that we can set and then we'll
     * set the rest once at least one proc from the required
     * leader policy becomes known
     */
    trp->leader_set = false;
    eval_policy(trp);

 release:
    /* release the triplet */
    OPAL_RELEASE_THREAD(&trp->lock, &trp->cond, &trp->in_use);

    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    return rc;
}


static int set_leader(const char *app,
                      const char *version,
                      const char *release,
                      const orte_process_name_t *leader)
{
    orcm_triplet_t *trp;

    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);

    /* find this triplet - create it if not found */
    trp = orcm_get_triplet(app, version, release, true);

    /* if the provided leader is NULL, reset this triplet
     * to follow its policy in selecting a leader
     */
    if (NULL == leader) {
        trp->leader_set = false;
        eval_policy(trp);
    } else {
        /* record the leader */
        trp->leader_set = true;
        trp->leader.jobid = leader->jobid;
        trp->leader.vpid = leader->vpid;
    }

    /* release the triplet */
    OPAL_RELEASE_THREAD(&trp->lock, &trp->cond, &trp->in_use);

    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    return ORCM_SUCCESS;
}

static bool deliver_msg(const char *stringid,
                        const orte_process_name_t *src,
                        const orte_rmcast_channel_t channel,
                        const orte_rmcast_seq_t seq_num)
{
    bool ret=false;
    orcm_triplet_t *trp;
    orcm_source_t *source;
    orcm_triplet_group_t *grp;
    int i;

    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);

    /* find this triplet - don't create it if not found */
    if (NULL == (trp = orcm_get_triplet_stringid(stringid))) {
        /* unknown */
        OPAL_OUTPUT_VERBOSE((0, orcm_leader_base.output,
                             "%s leader: stringid %s is unknown - can't deliver msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), stringid));
        /* can't deliver it */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return false;
    }

    /* do we need to set the leader? */
    if (!trp->leader_set) {
        eval_policy(trp);
    }

    /* get the group */
    grp = orcm_get_triplet_group(trp, src->jobid, true);

    /* if this is on the group output channel, then update the seq num
     * for this source
     */
    if (channel == grp->output) {
        source = orcm_get_source_in_group(grp, src->vpid, true);
        if (ORTE_RMCAST_SEQ_INVALID == source->seq_num) {
            /* initialize */
            source->seq_num = seq_num;
        } else {
            if (1 != (seq_num - source->seq_num)) {
                opal_output(0, "%s RECVD OUT-OF-ORDER MESSAGE: recvd %lu prior %lu",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            seq_num, source->seq_num);
            }
            source->seq_num = seq_num;
        }
        OPAL_RELEASE_THREAD(&source->lock, &source->cond, &source->in_use);
    }

    /* if the proc is within the defined leaders, let it thru */
    if (OPAL_EQUAL == orte_util_compare_name_fields((ORTE_NS_CMP_ALL|ORTE_NS_CMP_WILD), src, &trp->leader)) {
        OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                             "%s leader:lowest: %s is a leader for triplet %s - deliver msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(src), stringid));
        ret = true;
    } else {
        OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                             "%s leader:lowest: %s is not a leader for triplet %s - ignore msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(src), stringid));
    }

    OPAL_RELEASE_THREAD(&trp->lock, &trp->cond, &trp->in_use);
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    return ret;
}

static int get_leader(const char *app, const char *version,
                      const char *release, orte_process_name_t *leader)
{
    orcm_triplet_t *trp;

    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);

    /* find this triplet - create it if not found */
    trp = orcm_get_triplet(app, version, release, true);

    /* return the leader */
    leader->jobid = trp->leader.jobid;
    leader->vpid = trp->leader.vpid;

    /* done with triplet */
    OPAL_RELEASE_THREAD(&trp->lock, &trp->cond, &trp->in_use);

    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    return ORCM_SUCCESS;
}

static void proc_failed(const char *stringid, const orte_process_name_t *failed)
{
    orcm_triplet_t *trp;
    orte_process_name_t old_leader;
    bool notify=false;

    /* get the triplet corresponding to this process */
    if (NULL == (trp = orcm_get_triplet_stringid(stringid))) {
        /* unknown triplet */
        return;
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                         "%s PROC %s OF TRIPLET %s HAS FAILED",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(failed), stringid));

    /* do we need a new leader? */
    if (OPAL_EQUAL == orte_util_compare_name_fields((ORTE_NS_CMP_ALL|ORTE_NS_CMP_WILD), failed, &trp->leader)) {
        /* save the old leader */
        old_leader.jobid = failed->jobid;
        old_leader.vpid = failed->vpid;
        /* get a new one */
        eval_policy(trp);
    }

    /* check notification policy */
    if (ORCM_NOTIFY_NONE == trp->notify) {
        /* no notification - we are done */
        notify = false;
        OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                             "%s no failure notification requested",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    } else if (ORCM_NOTIFY_ANY & trp->notify) {
        /* notify when anyone fails */
        notify = true;
        OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                             "%s failure notificaton for ANY requested",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    } else if (ORCM_NOTIFY_GRP & trp->notify) {
        /* notify when failed proc is within group that
         * can lead
         */
        if (OPAL_EQUAL == orte_util_compare_name_fields((ORTE_NS_CMP_JOBID|ORTE_NS_CMP_WILD), failed, &trp->leader)) {
            notify = true;
            OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                                 "%s failure notification for GRP requested",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        }
    } else if (ORCM_NOTIFY_LDR & trp->notify) {
        /* notify if this proc was the leader */
        if (OPAL_EQUAL == orte_util_compare_name_fields((ORTE_NS_CMP_ALL|ORTE_NS_CMP_WILD), failed, &trp->leader)) {
            OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                                 "%s failure notification for LDR requested",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
            notify = true;
        }
    }

    if (notify && NULL != trp->leader_cbfunc) {
        OPAL_RELEASE_THREAD(&trp->lock, &trp->cond, &trp->in_use);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        /* pass back the old and new info */
        trp->leader_cbfunc(stringid, failed, &old_leader);
        return;
    }

    OPAL_RELEASE_THREAD(&trp->lock, &trp->cond, &trp->in_use);
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
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

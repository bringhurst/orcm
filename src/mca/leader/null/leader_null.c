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

#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"
#include "orte/util/name_fns.h"
#include "orte/threads/threads.h"

#include "mca/pnp/pnp.h"
#include "runtime/orcm_globals.h"
#include "util/triplets.h"

#include "mca/leader/leader.h"
#include "mca/leader/base/public.h"
#include "mca/leader/null/leader_null.h"

/* API functions */

static int null_init(void);
static void null_finalize(void);
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

orcm_leader_base_module_t orcm_leader_null_module = {
    null_init,
    null_finalize,
    set_policy,
    deliver_msg,
    set_leader,
    get_leader,
    proc_failed
};

/* local globals */
static orte_thread_ctl_t local_thread;


static int null_init(void)
{
    int ret;

    OBJ_CONSTRUCT(&local_thread, orte_thread_ctl_t);

    return ORCM_SUCCESS;
}

static void null_finalize(void)
{
    /* ensure any pending items complete */

    OBJ_DESTRUCT(&local_thread);
}

static void eval_policy(orcm_triplet_t *trp)
{
    orcm_triplet_group_t *grp;
    orcm_source_t *src;
    orte_process_name_t *policy;

    /* shorthand */
    policy = &trp->leader_policy;

    /* if the jobid is wildcard, then we allow messages
     * from any group to pass thru
     */
    if (ORTE_JOBID_WILDCARD == policy->jobid) {
        trp->leader.jobid = ORTE_JOBID_WILDCARD;
    } else if (ORTE_JOBID_INVALID == policy->jobid) {
        /* if invalid, then we only let messages from our
         * own group pass thru
         */
        trp->leader.jobid = ORTE_PROC_MY_NAME->jobid;
    } else {
        /* if the jobid is a specific value, then we only pass thru
         * messages from that group
         */
        trp->leader.jobid = policy->jobid;
    }

    /* if a specific vpid was given, then we use it - otherwise,
     * we let all vpids pass thru
     */
    if (ORTE_VPID_WILDCARD == policy->vpid ||
        ORTE_VPID_INVALID == policy->vpid) {
        trp->leader.vpid = ORTE_VPID_WILDCARD;
    } else {
        trp->leader.vpid = policy->vpid;
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

    ORTE_ACQUIRE_THREAD(&local_thread);

    OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                         "%s leader:null:set_policy for %s %s %s to %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release,
                         ORTE_NAME_PRINT(policy)));

    /* find this triplet - create it if not found */
    trp = orcm_get_triplet(app, version, release, true);

    /* if the policy is NULL, then this is being called for the purpose
     * of defining callback policy => cbfunc must be provided
     */
    if (NULL == policy) {
        if (NULL == cbfunc) {
            opal_output(0, "%s SET POLICY CALLED WITHOUT CBFUNC",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            return ORTE_ERR_BAD_PARAM;
        }
        /* set the cbfunc and leave the policy as default */
        trp->notify = notify;
        trp->leader_cbfunc = cbfunc;
        ORTE_RELEASE_THREAD(&local_thread);
        return ORCM_SUCCESS;
    }

    /* record the leadership policy */
    trp->leader_policy.jobid = policy->jobid;
    trp->leader_policy.vpid = policy->vpid;

    /* record the notify policy */
    trp->notify = notify;
    trp->leader_cbfunc = cbfunc;

    /* set the leader for this triplet if we can - at the
     * least, set the fields that we can set and then we'll
     * set the rest later
     */
    trp->leader_set = false;
    eval_policy(trp);

    /* release the triplet */
    ORTE_RELEASE_THREAD(&trp->ctl);

    ORTE_RELEASE_THREAD(&local_thread);
    return ORCM_SUCCESS;
}

static int set_leader(const char *app, const char *version,
                      const char *release,
                      const orte_process_name_t *leader)
{
    orcm_triplet_t *trp;

    ORTE_ACQUIRE_THREAD(&local_thread);

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
    ORTE_RELEASE_THREAD(&trp->ctl);

    ORTE_RELEASE_THREAD(&local_thread);
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

    ORTE_ACQUIRE_THREAD(&local_thread);

     /* find this triplet */
    if (NULL == (trp = orcm_get_triplet_stringid(stringid))) {
        OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                             "%s leader: stringid %s is unknown - can't deliver msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), stringid));
        /* can't deliver it */
        ORTE_RELEASE_THREAD(&local_thread);
        return false;
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
        ORTE_RELEASE_THREAD(&source->ctl);
    }

    /* if the proc is within the defined leaders, let it thru */
    if (OPAL_EQUAL == orte_util_compare_name_fields((ORTE_NS_CMP_ALL|ORTE_NS_CMP_WILD), src, &trp->leader)) {
        OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                             "%s leader:null: %s is a leader for triplet %s - deliver msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(src), stringid));
        ret = true;
        goto cleanup;
    }

 cleanup:
    ORTE_RELEASE_THREAD(&trp->ctl);
    ORTE_RELEASE_THREAD(&local_thread);
    return ret;
}

static int get_leader(const char *app, const char *version,
                      const char *release, orte_process_name_t *leader)
{
    orcm_triplet_t *trp;

    ORTE_ACQUIRE_THREAD(&local_thread);

      /* find this triplet - create it if not found */
    trp = orcm_get_triplet(app, version, release, true);

    /* return the leader */
    leader->jobid = trp->leader.jobid;
    leader->vpid = trp->leader.vpid;

    /* done with triplet */
    ORTE_RELEASE_THREAD(&trp->ctl);

    ORTE_RELEASE_THREAD(&local_thread);
    return ORCM_SUCCESS;
}

static void proc_failed(const char *stringid, const orte_process_name_t *failed)
{
    orcm_triplet_t *trp;
    int i;
    bool notify=false;

    ORTE_ACQUIRE_THREAD(&local_thread);

    /* find this triplet */
    if (NULL == (trp = orcm_get_triplet_stringid(stringid))) {
        /* unknown - ignore it */
        ORTE_RELEASE_THREAD(&local_thread);
        return;
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                         "%s PROC %s OF TRIPLET %s HAS FAILED",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(failed), stringid));

    /* this module does not support nor require re-evaluation
     * of leaders when a proc fails
     */

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
        ORTE_RELEASE_THREAD(&trp->ctl);
        ORTE_RELEASE_THREAD(&local_thread);
        /* pass back the old and new info */
        trp->leader_cbfunc(stringid, failed, &trp->leader);
        return;
    }

    ORTE_RELEASE_THREAD(&trp->ctl);
    ORTE_RELEASE_THREAD(&local_thread);
}

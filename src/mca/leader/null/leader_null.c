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
#include "mca/leader/null/leader_null.h"

/* API functions */

static int null_init(void);
static void null_finalize(void);
static bool deliver_msg(const char *stringid, const orte_process_name_t *src);
static int set_leader(const char *app, const char *version,
                      const char *release, const orte_vpid_t sibling,
                      orcm_leader_cbfunc_t cbfunc);
static int get_leader(const char *app, const char *version,
                      const char *release, orte_process_name_t *leader);
static void proc_failed(const char *stringid, const orte_process_name_t failed);

/* The module struct */

orcm_leader_base_module_t orcm_leader_null_module = {
    null_init,
    null_finalize,
    deliver_msg,
    set_leader,
    get_leader,
};

/* local globals */
static opal_mutex_t lock;
static opal_condition_t cond;
static bool active;


static int null_init(void)
{
    int ret;

    OBJ_CONSTRUCT(&lock, opal_mutex_t);
    OBJ_CONSTRUCT(&cond, opal_condition_t);
    active = false;

    return ORCM_SUCCESS;
}

static void null_finalize(void)
{
    /* ensure any pending items complete */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    OPAL_THREAD_UNLOCK(&lock);

    OBJ_DESTRUCT(&lock);
    OBJ_DESTRUCT(&cond);
}

static int set_leader(const char *app, const char *version,
                      const char *release, const orte_vpid_t sibling,
                      orcm_leader_cbfunc_t cbfunc)
{
    orcm_triplet_t *trp;
    orcm_source_t *src;

    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);

    OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                         "%s leader:null:set_leader for %s %s %s to %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release,
                         ORTE_VPID_PRINT(sibling)));

     /* find this triplet - create it if not found */
    trp = orcm_get_triplet(app, version, release, true);

    /* if the sibling specified here is INVALID, then we 
     * are being asked to set the leader using our algo. In
     * our case, this means just leave it alone - if it
     * was specified before, we don't want to override it
     */
    if (ORCM_LEADER_INVALID == sibling) {
        goto release;
    }
            
    /* do we know this source */
    if (NULL == (src = orcm_get_source(trp, sibling))) {
        /* nope - add it */
        src = OBJ_NEW(orcm_source_t);
        /* we don't know the jobid until they announce */
        src->name.vpid = sibling;
        src->alive = false;
        opal_pointer_array_set_item(&trp->members, sibling, src);
    }
    trp->leader.jobid = src->name.jobid;
    trp->leader.vpid = src->name.vpid;
    trp->leader_cbfunc = cbfunc;

    /* release the source */
    OPAL_RELEASE_THREAD(&src->lock, &src->cond, &src->in_use);

 release:
    /* release the triplet */
    OPAL_RELEASE_THREAD(&trp->lock, &trp->cond, &trp->in_use);

    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    return ORCM_SUCCESS;
}

static bool deliver_msg(const char *stringid, const orte_process_name_t *src)
{
    bool ret=false;
    orcm_triplet_t *trp;

    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);

     /* find this triplet */
    if (NULL == (trp = orcm_get_triplet_stringid(stringid))) {
        OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                             "%s leader: stringid %s is unknown - can't deliver msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), stringid));
        /* can't deliver it */
        return false;
    }

    /* if the triplet leader is wildcard, let it thru */
    if (ORTE_JOBID_WILDCARD == trp->leader.jobid &&
        ORTE_VPID_WILDCARD == trp->leader.vpid) {
        OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                             "%s leader: stringid %s is wildcard leader - deliver msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), stringid));
        ret = true;
        goto cleanup;
    }

    /* if the leader for this triplet was specified, then
     * check to see if we match
     */
    if ((ORTE_JOBID_WILDCARD == trp->leader.jobid || src->jobid == trp->leader.jobid) &&
        (ORTE_VPID_WILDCARD == trp->leader.vpid || src->vpid == trp->leader.vpid)) {
        ret = true;
    }

 cleanup:
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

static void proc_failed(const char *stringid, const orte_process_name_t failed)
{
    orcm_triplet_t *trp;
    int i;

    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);

    /* find this triplet */
    if (NULL == (trp = orcm_get_triplet_stringid(stringid))) {
        /* unknown - ignore it */
        goto cleanup;
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                         "%s PROC %s OF TRIPLET %s HAS FAILED",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(&failed), stringid));

    if (ORTE_VPID_WILDCARD != trp->leader.vpid &&
        failed.vpid == trp->leader.vpid) {
        /* there was a specific leader, and this was it - 
         * switch to default behavior
         */
        trp->leader.vpid = ORTE_VPID_WILDCARD;
    }

    if (NULL != trp->leader_cbfunc) {
        OPAL_RELEASE_THREAD(&trp->lock, &trp->cond, &trp->in_use);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        /* pass back the old and new info */
        trp->leader_cbfunc(stringid, failed, trp->leader);
        return;
    }

 cleanup:
    OPAL_RELEASE_THREAD(&trp->lock, &trp->cond, &trp->in_use);
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
}

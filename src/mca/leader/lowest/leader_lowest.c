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
static int set_leader(const char *app, const char *version,
                      const char *release, const orte_vpid_t sibling,
                      orcm_leader_cbfunc_t cbfunc);
static int get_leader(const char *app, const char *version,
                      const char *release, orte_process_name_t *leader);
static void proc_failed(const char *stringid, const orte_process_name_t failed);

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

static int set_leader(const char *app, const char *version,
                      const char *release, const orte_vpid_t sibling,
                      orcm_leader_cbfunc_t cbfunc)
{
    orcm_triplet_t *trp;
    orcm_source_t *src;
    orte_vpid_t i;

    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);

    OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                         "%s leader:lowest:set_leader for %s %s %s to %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release,
                         ORTE_VPID_PRINT(sibling)));

    /* find this triplet - create it if not found */
    trp = orcm_get_triplet(app, version, release, true);

    /* if the sibling specified here is INVALID, then we 
     * are being asked to set the leader using our algo
     */
    if (ORCM_LEADER_INVALID == sibling) {
        /* find the lowest vpid alive */
        for (i=0; i < trp->num_procs; i++) {
            if (NULL == (src = orcm_get_source(trp, i))) {
                continue;
            }
            if (src->alive) {
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
                goto release;
            }
            OPAL_RELEASE_THREAD(&src->lock, &src->cond, &src->in_use);
        }
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

 release:
    /* release the source */
    OPAL_RELEASE_THREAD(&src->lock, &src->cond, &src->in_use);

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
        OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                             "%s leader: stringid %s is unknown - can't deliver msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), stringid));
        ret = false;
    } else if (ORTE_JOBID_WILDCARD == trp->leader.jobid &&
               ORTE_VPID_WILDCARD == trp->leader.vpid) {
        OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                             "%s leader: stringid %s is wildcard leader - deliver msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), stringid));
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

static void proc_failed(const char *stringid, const orte_process_name_t failed)
{
    orcm_triplet_t *trp;
    orcm_source_t *src;
    int i;

    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);

    /* find this triplet */
    if (NULL == (trp = orcm_get_triplet_stringid(stringid))) {
        goto cleanup;
    }

    if (trp->leader.vpid == failed.vpid) {
        OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                             "%s LEADER %s FOR TRIPLET %s HAS FAILED",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&failed), stringid));
        /* find the lowest vpid still alive */
        for (i=0; i < trp->members.size; i++) {
            if (NULL == (src = orcm_get_source(trp, i))) {
                continue;
            }
            if (src->name.jobid != failed.jobid ||
                src->name.vpid != failed.vpid) {
                OPAL_RELEASE_THREAD(&src->lock, &src->cond, &src->in_use);
                continue;
            }
            if (src->alive) {
                trp->leader.jobid = src->name.jobid;
                trp->leader.vpid = src->name.vpid;
                OPAL_RELEASE_THREAD(&src->lock, &src->cond, &src->in_use);
                break;
            }
        }
    } else {
        OPAL_OUTPUT_VERBOSE((2, orcm_leader_base.output,
                             "%s NON-LEADER %s FOR TRIPLET %s HAS FAILED",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&failed), stringid));
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


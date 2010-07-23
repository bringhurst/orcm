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

#include "orte/mca/rml/rml.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"

#include "mca/pnp/base/private.h"
#include "runtime/runtime.h"

#include "mca/leader/leader.h"
#include "mca/leader/base/private.h"
#include "mca/leader/lowest/leader_lowest.h"

/* API functions */

static int lowest_init(void);
static void lowest_finalize(void);
static bool deliver_msg(orte_process_name_t *src);
static int set_leader(char *app,
                      char *version,
                      char *release,
                      orte_vpid_t sibling,
                      orcm_leader_cbfunc_t cbfunc);
static int get_leader(char *app,
                      char *version,
                      char *release,
                      orte_process_name_t *leader);

/* The module struct */

orcm_leader_base_module_t orcm_leader_lowest_module = {
    lowest_init,
    lowest_finalize,
    deliver_msg,
    set_leader,
    get_leader,
};

/* local globals */
static opal_list_t triplets;

static void proc_failed(int status, orte_process_name_t* sender,
                        opal_buffer_t* buffer, orte_rml_tag_t tag,
                        void* cbdata);

static int lowest_init(void)
{
    int ret;

    /* if we are an app, track our known triplets so we can
     * callback the right function when someone fails
     */
    if (ORCM_PROC_IS_APP) {
        OBJ_CONSTRUCT(&triplets, opal_list_t);

        /* setup a recv by which the orcmd can tell us someone failed */
        if (ORTE_SUCCESS != (ret = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD,
                                                           ORTE_RML_TAG_LEADER,
                                                           ORTE_RML_PERSISTENT,
                                                           proc_failed,
                                                           NULL))) {
            if (ORTE_EXISTS != ret) {
                ORTE_ERROR_LOG(ret);
                return ret;
            }
        }
    }

    return ORCM_SUCCESS;
}

static void lowest_finalize(void)
{
    opal_list_item_t *item;

    if (ORCM_PROC_IS_APP) {
        while (NULL != (item = opal_list_remove_first(&triplets))) {
            OBJ_RELEASE(item);
        }
        OBJ_DESCTRUCT(&triplets);
        orte_rml.recv_cancel(ORTE_NAME_WILDCARD, ORTE_RML_TAG_LEADER);
    }
}

static int set_leader(char *app,
                      char *version,
                      char *release,
                      orte_vpid_t sibling,
                      orcm_leader_cbfunc_t cbfunc)
{
    char *stringid;
    orcm_leader_t *trp;
    opal_list_item_t *item;

    OPAL_OUTPUT_VERBOSE((0, orcm_leader_base.output,
                         "%s leader:lowest:set_leader for %s %s %s to %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         app, version, release,
                         ORTE_VPID_PRINT(sibling)));

    /* if a cbfunc was provided, record it */
    if (NULL != cbfunc) {
        ORCM_PNP_CREATE_STRING_ID(&stringid, app, version, release);
        /* see if we already have this triplet */
        for (item = opal_list_get_first(&triplets);
             item != opal_list_get_end(&triplets);
             item = opal_list_get_next(item)) {
            trp = (orcm_leader_t*)item;
            if (0 == strcasecmp(stringid, trp->stringid)) {
                /* update cbfunc */
                trp->cbfunc = cbfunc;
                return ORCM_SUCCESS;
            }
        }
        /* get here if not found - create one */
        trp = OBJ_NEW(orcm_leader_t);
        trp->stringid = strdup(stringid);
        trp->cbfunc = cbfunc;
        opal_list_append_item(&triplets, &trp->super);
    }

    /* this module has no leader */
    return ORCM_SUCCESS;
}

static bool deliver_msg(orte_process_name_t *src)
{
    return true;
}

static int get_leader(char *app,
                      char *version,
                      char *release,
                      orte_process_name_t *leader)
{
    leader->jobid = ORTE_NAME_INVALID->jobid;
    leader->vpid = ORTE_NAME_INVALID->vpid;
    return ORTE_SUCCESS;
}

static void proc_failed(int status, orte_process_name_t* sender,
                        opal_buffer_t* buffer, orte_rml_tag_t tag,
                        void* cbdata)
{
}


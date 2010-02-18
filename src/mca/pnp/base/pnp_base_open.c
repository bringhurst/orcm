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

#include "opal/util/output.h"
#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"

#include "orte/mca/rml/rml_types.h"
#include "orte/mca/rmcast/rmcast_types.h"

#include "mca/pnp/pnp.h"
#include "mca/pnp/base/public.h"
#include "mca/pnp/base/private.h"
#include "mca/pnp/base/components.h"

const mca_base_component_t *orcm_pnp_base_components[] = {
    &mca_pnp_default_component.pnpc_version,
    NULL
};

/* instantiate the module */
orcm_pnp_base_module_t orcm_pnp = {
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

/* instantiate the wildcard source */
orcm_pnp_source_t orcm_pnp_wildcard;

/* instantiate the globals */
orcm_pnp_base_t orcm_pnp_base;

int orcm_pnp_base_open(void)
{
    /* setup the source wildcard */
    orcm_pnp_wildcard.name.jobid = ORTE_JOBID_WILDCARD;
    orcm_pnp_wildcard.name.vpid = ORTE_VPID_WILDCARD;
    
    /* Debugging / verbose output.  Always have stream open, with
     verbose set by the mca open system... */
    orcm_pnp_base.output = opal_output_open(NULL);
    
    /* Open up all available components */
    if (ORCM_SUCCESS != 
        mca_base_components_open("pnp", orcm_pnp_base.output,
                                 orcm_pnp_base_components, 
                                 &orcm_pnp_base.opened, true)) {
            return ORCM_ERROR;
        }
    
    /* All done */
    return ORCM_SUCCESS;
}

/****    INSTANTIATE CLASSES    ****/
static void source_constructor(orcm_pnp_source_t *ptr)
{
    ptr->name.jobid = ORTE_JOBID_INVALID;
    ptr->name.vpid = ORTE_VPID_INVALID;
    ptr->failed = false;
    memset(ptr->msgs, 0, ORCM_PNP_MAX_MSGS*sizeof(opal_buffer_t*));
    ptr->start = 0;
    ptr->end = 0;
    ptr->last_msg_num = ORTE_RMCAST_SEQ_INVALID;
}
static void source_destructor(orcm_pnp_source_t *ptr)
{
    int i;
    
    for (i=0; i < ORCM_PNP_MAX_MSGS; i++) {
        if (NULL != ptr->msgs[i]) {
            OBJ_RELEASE(ptr->msgs[i]);
        }
    }
}
OBJ_CLASS_INSTANCE(orcm_pnp_source_t,
                   opal_object_t,
                   source_constructor,
                   source_destructor);

static void group_constructor(orcm_pnp_group_t *ptr)
{
    ptr->app = NULL;
    ptr->version = NULL;
    ptr->release = NULL;
    ptr->channel = ORTE_RMCAST_INVALID_CHANNEL;
    OBJ_CONSTRUCT(&ptr->members, opal_pointer_array_t);
    opal_pointer_array_init(&ptr->members, 8, INT_MAX, 8);
    OBJ_CONSTRUCT(&ptr->requests, opal_pointer_array_t);
    opal_pointer_array_init(&ptr->requests, 8, INT_MAX, 8);
}
static void group_destructor(orcm_pnp_group_t *ptr)
{
    int i;
    orcm_pnp_pending_request_t *req;
    orcm_pnp_source_t *src;
    
    if (NULL != ptr->app) {
        free(ptr->app);
    }
    if (NULL != ptr->version) {
        free(ptr->version);
    }
    if (NULL != ptr->release) {
        free(ptr->release);
    }
    for (i=0; i < ptr->members.size; i++) {
        if (NULL != (src = (orcm_pnp_source_t*)opal_pointer_array_get_item(&ptr->members, i))) {
            OBJ_RELEASE(src);
        }
    }
    OBJ_DESTRUCT(&ptr->members);
    for (i=0; i < ptr->requests.size; i++) {
        if (NULL != (req = (orcm_pnp_pending_request_t*)opal_pointer_array_get_item(&ptr->requests, i))) {
            OBJ_RELEASE(req);
        }
    }
    OBJ_DESTRUCT(&ptr->requests);
}
OBJ_CLASS_INSTANCE(orcm_pnp_group_t,
                   opal_object_t,
                   group_constructor,
                   group_destructor);

static void tracker_constructor(orcm_pnp_channel_tracker_t *ptr)
{
    ptr->app = NULL;
    ptr->version = NULL;
    ptr->release = NULL;
    ptr->channel = ORCM_PNP_INVALID_CHANNEL;
    OBJ_CONSTRUCT(&ptr->groups, opal_pointer_array_t);
    opal_pointer_array_init(&ptr->groups, 8, INT_MAX, 8);
    OBJ_CONSTRUCT(&ptr->requests, opal_pointer_array_t);
    opal_pointer_array_init(&ptr->requests, 8, INT_MAX, 8);
}
static void tracker_destructor(orcm_pnp_channel_tracker_t *ptr)
{
    int i;
    orcm_pnp_group_t *grp;
    orcm_pnp_pending_request_t *req;

    if (NULL != ptr->app) {
        free(ptr->app);
    }
    if (NULL != ptr->version) {
        free(ptr->version);
    }
    if (NULL != ptr->release) {
        free(ptr->release);
    }
    for (i=0; i < ptr->groups.size; i++) {
        if (NULL != (grp = (orcm_pnp_group_t*)opal_pointer_array_get_item(&ptr->groups, i))) {
            OBJ_RELEASE(grp);
        }
    }
    OBJ_DESTRUCT(&ptr->groups);
    for (i=0; i < ptr->requests.size; i++) {
        if (NULL != (req = (orcm_pnp_pending_request_t*)opal_pointer_array_get_item(&ptr->requests, i))) {
            OBJ_RELEASE(req);
        }
    }
    OBJ_DESTRUCT(&ptr->requests);
}
OBJ_CLASS_INSTANCE(orcm_pnp_channel_tracker_t,
                   opal_object_t,
                   tracker_constructor,
                   tracker_destructor);

static void request_constructor(orcm_pnp_pending_request_t *ptr)
{
    ptr->tag = ORCM_PNP_TAG_WILDCARD;
    ptr->cbfunc = NULL;
    ptr->cbfunc_buf = NULL;
}
/* no destruct required here */
OBJ_CLASS_INSTANCE(orcm_pnp_pending_request_t,
                   opal_list_item_t,
                   request_constructor, NULL);

static void recv_constructor(orcm_pnp_recv_t *ptr)
{
    ptr->grp = NULL;
    ptr->src = NULL;
    ptr->channel = ORTE_RMCAST_INVALID_CHANNEL;
    ptr->tag = ORTE_RML_TAG_INVALID;
    ptr->msg = NULL;
    ptr->count = 0;
    ptr->cbfunc = NULL;
    ptr->buffer = NULL;
    ptr->cbfunc_buf = NULL;
    ptr->cbdata = NULL;
}
OBJ_CLASS_INSTANCE(orcm_pnp_recv_t,
                   opal_list_item_t,
                   recv_constructor,
                   NULL);

static void send_constructor(orcm_pnp_send_t *ptr)
{
    ptr->target.jobid = ORTE_JOBID_INVALID;
    ptr->target.vpid = ORTE_VPID_INVALID;
    ptr->pending = false;
    OBJ_CONSTRUCT(&ptr->lock, opal_mutex_t);
    OBJ_CONSTRUCT(&ptr->cond, opal_condition_t);
    ptr->channel = ORTE_RMCAST_INVALID_CHANNEL;
    ptr->tag = ORTE_RML_TAG_INVALID;
    ptr->msg = NULL;
    ptr->count = 0;
    ptr->cbfunc = NULL;
    ptr->buffer = NULL;
    ptr->cbfunc_buf = NULL;
    ptr->cbdata = NULL;
}
static void send_destructor(orcm_pnp_send_t *ptr) {
    OBJ_DESTRUCT(&ptr->lock);
    OBJ_DESTRUCT(&ptr->cond);
}
OBJ_CLASS_INSTANCE(orcm_pnp_send_t,
                   opal_list_item_t,
                   send_constructor,
                   send_destructor);

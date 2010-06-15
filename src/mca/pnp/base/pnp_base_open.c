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
#include "opal/class/opal_pointer_array.h"
#include "opal/util/output.h"
#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"

#include "orte/mca/rml/rml_types.h"
#include "orte/mca/rmcast/rmcast_types.h"

#include "mca/pnp/pnp.h"
#include "mca/pnp/base/public.h"
#include "mca/pnp/base/private.h"
#include "mca/pnp/base/components.h"

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
        mca_base_components_open("orcm_pnp", orcm_pnp_base.output, NULL,
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

static void triplet_constructor(orcm_pnp_triplet_t *ptr)
{
    ptr->string_id = NULL;
    ptr->output = ORTE_RMCAST_INVALID_CHANNEL;
    ptr->input = ORTE_RMCAST_INVALID_CHANNEL;
    ptr->cbfunc = NULL;
    OBJ_CONSTRUCT(&ptr->members, opal_pointer_array_t);
    opal_pointer_array_init(&ptr->members, 8, INT_MAX, 8);
    OBJ_CONSTRUCT(&ptr->input_recvs, opal_list_t);
    OBJ_CONSTRUCT(&ptr->output_recvs, opal_list_t);
}
static void triplet_destructor(orcm_pnp_triplet_t *ptr)
{
    opal_list_item_t *item;

    if (NULL != ptr->string_id) {
        free(ptr->string_id);
    }
    while (NULL != (item = opal_list_remove_first(&ptr->input_recvs))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&ptr->input_recvs);
    while (NULL != (item = opal_list_remove_first(&ptr->output_recvs))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&ptr->output_recvs);
}
OBJ_CLASS_INSTANCE(orcm_pnp_triplet_t,
                   opal_object_t,
                   triplet_constructor,
                   triplet_destructor);

static void request_constructor(orcm_pnp_request_t *ptr)
{
    ptr->string_id = NULL;
    ptr->tag = ORCM_PNP_TAG_WILDCARD;
    ptr->cbfunc = NULL;
}
static void request_destructor(orcm_pnp_request_t *ptr)
{
    if (NULL != ptr->string_id) {
        free(ptr->string_id);
    }
}
/* no destruct required here */
OBJ_CLASS_INSTANCE(orcm_pnp_request_t,
                   opal_list_item_t,
                   request_constructor,
                   request_destructor);

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

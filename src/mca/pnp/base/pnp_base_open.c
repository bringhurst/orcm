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
#include "opal/mca/base/mca_base_param.h"

#include "orte/mca/rml/rml_types.h"
#include "orte/mca/rmcast/rmcast_types.h"

#include "mca/pnp/pnp.h"
#include "mca/pnp/base/public.h"
#include "mca/pnp/base/private.h"

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
    NULL
};

/* instantiate the globals */
orcm_pnp_base_t orcm_pnp_base;

int orcm_pnp_base_open(void)
{
    int tmp;

    /* Debugging / verbose output.  Always have stream open, with
     * verbose set by the mca open system...
     */
    orcm_pnp_base.output = opal_output_open(NULL);
    
    mca_base_param_reg_int_name("pnp", "use_threads",
                                "Use progress threads (default: 0)",
                                   false, false, 0, &tmp);
    orcm_pnp_base.use_threads = OPAL_INT_TO_BOOL(tmp);

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
static void channel_constructor(orcm_pnp_channel_obj_t *ptr)
{
    ptr->channel = ORCM_PNP_INVALID_CHANNEL;
    OBJ_CONSTRUCT(&ptr->recvs, opal_list_t);
}
static void channel_destructor(orcm_pnp_channel_obj_t *ptr)
{
    opal_list_item_t *item;

    while (NULL != (item = opal_list_remove_first(&ptr->recvs))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&ptr->recvs);
}
OBJ_CLASS_INSTANCE(orcm_pnp_channel_obj_t,
                   opal_object_t,
                   channel_constructor,
                   channel_destructor);

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

static void msg_constructor(orcm_pnp_msg_t *ptr)
{
    ptr->sender.jobid = ORTE_JOBID_INVALID;
    ptr->sender.vpid = ORTE_VPID_INVALID;
    ptr->tag = ORCM_PNP_TAG_INVALID;
    OBJ_CONSTRUCT(&ptr->buf, opal_buffer_t);
    ptr->cbfunc = NULL;
}
static void msg_destructor(orcm_pnp_msg_t *ptr)
{
    OBJ_DESTRUCT(&ptr->buf);
}
OBJ_CLASS_INSTANCE(orcm_pnp_msg_t,
                   opal_list_item_t,
                   msg_constructor,
                   msg_destructor);

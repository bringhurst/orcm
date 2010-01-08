/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef PNP_BASE_PRIVATE_H
#define PNP_BASE_PRIVATE_H

#include "openrcm.h"

#include "opal/dss/dss_types.h"
#include "opal/class/opal_list.h"
#include "opal/class/opal_value_array.h"

#include "orte/mca/rml/rml_types.h"
#include "orte/mca/rmcast/rmcast_types.h"

#include "mca/pnp/pnp_types.h"

BEGIN_C_DECLS

/*
 * globals that might be needed
 */
typedef struct {
    int output;
    opal_list_t opened;
} orcm_pnp_base_t;

ORCM_DECLSPEC extern orcm_pnp_base_t orcm_pnp_base;

typedef struct {
    opal_list_item_t super;
    orcm_pnp_tag_t tag;
    orcm_pnp_callback_fn_t cbfunc;
    orcm_pnp_callback_buffer_fn_t cbfunc_buf;
} orcm_pnp_pending_request_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_pending_request_t);

typedef struct {
    opal_object_t super;
    orte_rml_tag_t tag;
    opal_buffer_t *buffer;
    orcm_pnp_callback_buffer_fn_t cbfunc_buf;
    struct iovec *msg;
    int count;
    orcm_pnp_callback_fn_t cbfunc;
    void *cbdata;
} orcm_pnp_send_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_send_t);

ORCM_DECLSPEC void orcm_pnp_base_push_data(orcm_pnp_source_t *src, opal_buffer_t *buf);
ORCM_DECLSPEC opal_buffer_t* orcm_pnp_base_pop_data(orcm_pnp_source_t *src);

END_C_DECLS

#endif

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
    opal_mutex_t lock;
    opal_condition_t cond;
    opal_buffer_t msgs;
    bool msg_pending;
} orcm_pnp_heartbeat_t;
ORCM_DECLSPEC extern orcm_pnp_heartbeat_t orcm_pnp_heartbeat;

typedef struct {
    opal_list_item_t super;
    orcm_pnp_tag_t tag;
    orcm_pnp_callback_fn_t cbfunc;
    orcm_pnp_callback_buffer_fn_t cbfunc_buf;
} orcm_pnp_pending_request_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_pending_request_t);

typedef struct {
    opal_object_t super;
    char *app;
    char *version;
    char *release;
    orcm_pnp_channel_t channel;
    opal_pointer_array_t groups;
    opal_list_t requests;
} orcm_pnp_channel_tracker_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_channel_tracker_t);

END_C_DECLS

#endif

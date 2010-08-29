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

#include "orte/mca/rml/rml_types.h"
#include "orte/mca/rmcast/rmcast_types.h"

#include "mca/pnp/pnp_types.h"

BEGIN_C_DECLS

#define ORCM_PNP_MAX_MSGS    8

/*
 * globals that might be needed
 */
typedef struct {
    int output;
    opal_list_t opened;
    int max_msgs;
} orcm_pnp_base_t;
ORCM_DECLSPEC extern orcm_pnp_base_t orcm_pnp_base;

typedef struct {
    opal_list_item_t super;
    char *string_id;
    orcm_pnp_tag_t tag;
    orcm_pnp_callback_fn_t cbfunc;
} orcm_pnp_request_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_request_t);

typedef struct {
    opal_object_t super;
    orcm_pnp_channel_t channel;
    opal_list_t recvs;
} orcm_pnp_channel_obj_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_channel_obj_t);

typedef struct {
    opal_list_item_t super;
    orte_process_name_t target;
    bool pending;
    opal_mutex_t lock;
    opal_condition_t cond;
    orte_rmcast_channel_t channel;
    orte_rmcast_tag_t tag;
    struct iovec *msg;
    int count;
    orcm_pnp_callback_fn_t cbfunc;
    opal_buffer_t *buffer;
    void *cbdata;
} orcm_pnp_send_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_send_t);

typedef struct {
    opal_list_item_t super;
    orte_process_name_t sender;
    orcm_pnp_tag_t tag;
    opal_buffer_t buf;
    orcm_pnp_callback_fn_t cbfunc;
} orcm_pnp_msg_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_msg_t);

/* internal base functions */
ORCM_DECLSPEC char* orcm_pnp_print_tag(orcm_pnp_tag_t tag);
ORCM_DECLSPEC char* orcm_pnp_print_channel(orcm_pnp_channel_t chan);

END_C_DECLS

#endif

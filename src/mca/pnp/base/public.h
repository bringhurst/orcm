/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef PNP_BASE_PUBLIC_H
#define PNP_BASE_PUBLIC_H
#include "openrcm_config_private.h"

#include "opal/class/opal_list.h"
#include "opal/class/opal_pointer_array.h"

#include "orte/threads/threads.h"

#include "mca/pnp/pnp.h"

BEGIN_C_DECLS

typedef struct {
    int output;
    opal_list_t opened;
    int recv_pipe[2];
    opal_thread_t recv_process;
    orte_thread_ctl_t recv_process_ctl;
    uint32_t my_uid;
    char *my_string_id;
    orcm_triplet_t *my_triplet;
    orcm_triplet_group_t *my_group;
    orcm_pnp_announce_fn_t my_announce_cbfunc;
    orcm_pnp_channel_obj_t *my_input_channel;
    orcm_pnp_channel_obj_t *my_output_channel;
    opal_pointer_array_t channels;
    bool comm_enabled;
} orcm_pnp_base_t;
ORCM_DECLSPEC extern orcm_pnp_base_t orcm_pnp_base;

ORCM_DECLSPEC int orcm_pnp_base_open(void);
ORCM_DECLSPEC int orcm_pnp_base_select(void);
ORCM_DECLSPEC int orcm_pnp_base_close(void);
ORCM_DECLSPEC void orcm_pnp_print_buffer_finalize(void);

ORCM_DECLSPEC extern const mca_base_component_t *orcm_pnp_base_components[];

END_C_DECLS

#endif

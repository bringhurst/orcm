#ifndef PNP_BASE_PRIVATE_H
#define PNP_BASE_PRIVATE_H

#include "openrcm_config.h"

#include "opal/dss/dss_types.h"
#include "opal/class/opal_list.h"
#include "opal/class/opal_value_array.h"

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
    orcm_pnp_callback_fn_t cbfunc;
    orcm_pnp_callback_buffer_fn_t cbfunc_buf;
    void *cbdata;
} orcm_pnp_send_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_send_t);

ORCM_DECLSPEC void orcm_pnp_base_push_data(orcm_pnp_source_t *src, opal_buffer_t *buf);
ORCM_DECLSPEC opal_buffer_t* orcm_pnp_base_pop_data(orcm_pnp_source_t *src);
ORCM_DECLSPEC bool orcm_pnp_base_valid_sequence_number(orcm_pnp_source_t *src,
                                                       orte_rmcast_seq_t seq);

END_C_DECLS

#endif

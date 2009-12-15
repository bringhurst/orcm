/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef ORCM_PNP_TYPES_H
#define ORCM_PNP_TYPES_H

#include "openrcm_config.h"

#include "opal/dss/dss_types.h"
#include "opal/class/opal_list.h"
#include "opal/class/opal_value_array.h"

#include "orte/types.h"
#include "orte/mca/rmcast/rmcast_types.h"

#include "mca/leader/leader_types.h"

#define ORCM_PNP_MAX_MSGS    4

typedef struct {
    opal_list_item_t super;
    orte_process_name_t name;
    bool failed;
    opal_buffer_t *msgs[ORCM_PNP_MAX_MSGS];
    int start, end;
    orte_rmcast_seq_t last_msg_num;
} orcm_pnp_source_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_source_t);

/* provide a wildcard version */
ORCM_DECLSPEC extern orcm_pnp_source_t orcm_pnp_wildcard;
#define ORCM_SOURCE_WILDCARD    (&orcm_pnp_wildcard)


typedef struct {
    opal_list_item_t super;
    char *app;
    char *version;
    char *release;
    char *string_id;
    opal_value_array_t channels;
    opal_list_t requests;
    opal_list_t members;
    orcm_pnp_source_t *leader;
    bool leader_set;
    orcm_leader_cbfunc_t leader_failed_cbfunc;
} orcm_pnp_group_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_group_t);

typedef int32_t orcm_pnp_tag_t;
#define ORCM_PNP_TAG_T  OPAL_INT32

/* inherited tag values */
enum {
    ORCM_PNP_TAG_WILDCARD       = ORTE_RMCAST_TAG_WILDCARD,
    ORCM_PNP_TAG_INVALID        = ORTE_RMCAST_TAG_INVALID,
    ORCM_PNP_TAG_BOOTSTRAP      = ORTE_RMCAST_TAG_BOOTSTRAP,
    ORCM_PNP_TAG_ANNOUNCE       = ORTE_RMCAST_TAG_ANNOUNCE,
    ORCM_PNP_TAG_OUTPUT         = ORTE_RMCAST_TAG_OUTPUT,
    ORCM_PNP_TAG_PS             = ORTE_RMCAST_TAG_PS
};

#define ORCM_PNP_TAG_DYNAMIC    100

#endif /* ORCM_PNP_TYPES_H */

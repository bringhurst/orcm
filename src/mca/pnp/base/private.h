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

#define ORCM_PNP_CREATE_STRING_ID(sid, a, v, r) \
    do {                                        \
        asprintf((sid), "%s:%s:%s",             \
                 (NULL == (a)) ? "@" : (a),     \
                 (NULL == (v)) ? "@" : (v),     \
                 (NULL == (r)) ? "@" : (r));    \
    } while(0);

#define ORCM_PNP_DECOMPOSE_STRING_ID(sid, a, v, r)  \
    do {                                            \
        char *c, *c2, *t;                           \
        t = strdup((sid));                          \
        c = strchr(t, ':');                         \
        *c = '\0';                                  \
        if (0 == strcmp(t, "@")) {                  \
            (a) = NULL;                             \
        } else {                                    \
            (a) = strdup(t);                        \
        }                                           \
        c++;                                        \
        c2 = strchr(c, ':');                        \
        *c2 = '\0';                                 \
        if (0 == strcmp(c, "@")) {                  \
            (v) = NULL;                             \
        } else {                                    \
            (v) = strdup(c);                        \
        }                                           \
        c2++;                                       \
        if (0 == strcmp(c2, "@")) {                 \
            (r) = NULL;                             \
        } else {                                    \
            (r) = strdup(c2);                       \
        }                                           \
        free(t);                                    \
    } while(0);

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
    char *string_id;
    orcm_pnp_tag_t tag;
    orcm_pnp_callback_fn_t cbfunc;
} orcm_pnp_request_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_request_t);

typedef struct {
    opal_object_t super;
    char *string_id;
    orcm_pnp_channel_t output;
    orcm_pnp_channel_t input;
    orcm_pnp_open_channel_cbfunc_t cbfunc;
    opal_pointer_array_t members;
    opal_list_t input_recvs;
    opal_list_t output_recvs;
} orcm_pnp_triplet_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_triplet_t);

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
    opal_object_t super;
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


END_C_DECLS

#endif

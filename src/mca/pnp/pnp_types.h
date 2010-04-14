/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef ORCM_PNP_TYPES_H
#define ORCM_PNP_TYPES_H

#include "openrcm.h"

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#include "opal/dss/dss_types.h"
#include "opal/class/opal_list.h"
#include "opal/class/opal_pointer_array.h"
#include "opal/threads/condition.h"
#include "opal/threads/mutex.h"

#include "orte/types.h"
#include "orte/mca/rmcast/rmcast_types.h"

#include "mca/leader/leader_types.h"

#define ORCM_PNP_MAX_MSGS    4

typedef int32_t orcm_pnp_tag_t;
#define ORCM_PNP_TAG_T  OPAL_INT32

/* inherited tag values */
enum {
    ORCM_PNP_TAG_WILDCARD       = ORTE_RMCAST_TAG_WILDCARD,
    ORCM_PNP_TAG_INVALID        = ORTE_RMCAST_TAG_INVALID,
    ORCM_PNP_TAG_BOOTSTRAP      = ORTE_RMCAST_TAG_BOOTSTRAP,
    ORCM_PNP_TAG_ANNOUNCE       = ORTE_RMCAST_TAG_ANNOUNCE,
    ORCM_PNP_TAG_OUTPUT         = ORTE_RMCAST_TAG_OUTPUT,
    ORCM_PNP_TAG_PS             = ORTE_RMCAST_TAG_PS,
    ORCM_PNP_TAG_MSG            = ORTE_RMCAST_TAG_MSG,
    ORCM_PNP_TAG_TOOL           = ORTE_RMCAST_TAG_TOOL,
    ORCM_PNP_TAG_IOF            = ORTE_RMCAST_TAG_IOF,
    ORCM_PNP_HEARTBEAT,
    ORCM_PNP_TAG_COMMAND
};

#define ORCM_PNP_TAG_DYNAMIC    100

typedef int32_t orcm_pnp_channel_t;
#define ORCM_PNP_CHANNEL_T  OPAL_INT32

/* inherited channels */
enum {
    ORCM_PNP_GROUP_OUTPUT_CHANNEL   = ORTE_RMCAST_GROUP_OUTPUT_CHANNEL,
    ORCM_PNP_WILDCARD_CHANNEL       = ORTE_RMCAST_WILDCARD_CHANNEL,
    ORCM_PNP_INVALID_CHANNEL        = ORTE_RMCAST_INVALID_CHANNEL,
    ORCM_PNP_SYS_CHANNEL            = ORTE_RMCAST_SYS_CHANNEL,
    ORCM_PNP_APP_PUBLIC_CHANNEL     = ORTE_RMCAST_APP_PUBLIC_CHANNEL
};

#define ORCM_PNP_DYNAMIC_CHANNELS   ORTE_RMCAST_DYNAMIC_CHANNELS

/* callback functions */
typedef void (*orcm_pnp_announce_fn_t)(char *app, char *version, char *release,
                                       orte_process_name_t *name, char *node);

typedef void (*orcm_pnp_callback_fn_t)(int status,
                                       orte_process_name_t *sender,
                                       orcm_pnp_tag_t tag,
                                       struct iovec *msg,
                                       int count,
                                       void *cbdata);

typedef void (*orcm_pnp_callback_buffer_fn_t)(int status,
                                              orte_process_name_t *sender,
                                              orcm_pnp_tag_t tag,
                                              opal_buffer_t *buf,
                                              void *cbdata);

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


typedef struct {
    opal_object_t super;
    char *app;
    char *version;
    char *release;
    char *string_id;
    orte_rmcast_channel_t channel;
    opal_pointer_array_t members;
    opal_list_t requests;
} orcm_pnp_group_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_group_t);

typedef struct {
    opal_list_item_t super;
    bool pending;
    opal_mutex_t lock;
    opal_condition_t cond;
    orcm_pnp_group_t *grp;
    orcm_pnp_source_t *src;
    orte_rmcast_channel_t channel;
    orte_rmcast_tag_t tag;
    struct iovec *msg;
    int count;
    orcm_pnp_callback_fn_t cbfunc;
    opal_buffer_t *buffer;
    orcm_pnp_callback_buffer_fn_t cbfunc_buf;
    void *cbdata;
} orcm_pnp_recv_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_recv_t);

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
    orcm_pnp_callback_buffer_fn_t cbfunc_buf;
    void *cbdata;
} orcm_pnp_send_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_send_t);

#endif /* ORCM_PNP_TYPES_H */

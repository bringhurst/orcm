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

#include "opal/dss/dss_types.h"
#include "opal/class/opal_list.h"
#include "opal/class/opal_value_array.h"
#include "opal/threads/condition.h"
#include "opal/threads/mutex.h"

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

typedef struct {
    opal_list_item_t super;
    orcm_pnp_group_t *grp;
    orcm_pnp_source_t *src;
    orte_rmcast_channel_t channel;
    orte_rmcast_tag_t tag;
    struct iovec *msg;
    int count;
    opal_buffer_t *buffer;
    void *cbdata;
} orcm_msg_packet_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_msg_packet_t);

#define ORCM_PROCESS_PNP_IOVECS(rlist, lck, cond, gp, sndr, \
                                chan, tg, mg, cnt, cbd)     \
    do {                                                    \
        orcm_msg_packet_t *pkt;                             \
        pkt = OBJ_NEW(orcm_msg_packet_t);                   \
        pkt->grp = (gp);                                    \
        pkt->src = (sndr);                                  \
        pkt->channel = (chan);                              \
        pkt->tag = (tg);                                    \
        pkt->msg = (mg);                                    \
        pkt->count = (cnt);                                 \
        pkt->cbdata = (cbd);                                \
        OPAL_THREAD_LOCK((lck));                            \
        opal_list_append((rlist), &pkt->super);             \
        opal_condition_broadcast((cond));                   \
        OPAL_THREAD_UNLOCK((lck));                          \
    } while(0);

#define ORCM_PROCESS_PNP_BUFFERS(rlist, lck, cond, gp, sndr,    \
                                 chan, tg, buf, cbd)            \
    do {                                                        \
        orcm_msg_packet_t *pkt;                                 \
        pkt = OBJ_NEW(orcm_msg_packet_t);                       \
        pkt->grp = (gp);                                        \
        pkt->src = (sndr);                                      \
        pkt->channel = (chan);                                  \
        pkt->tag = (tg);                                        \
        pkt->buffer = OBJ_NEW(opal_buffer_t);                   \
        opal_dss.copy_payload(pkt->buffer, (buf));              \
        OPAL_THREAD_LOCK((lck));                                \
        opal_list_append((rlist), &pkt->super);                 \
        opal_condition_broadcast((cond));                       \
        OPAL_THREAD_UNLOCK((lck));                              \
    } while(0);

#endif /* ORCM_PNP_TYPES_H */

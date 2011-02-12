/*
 * Copyright (c) 2009-2011 Cisco Systems, Inc.  All rights reserved. 
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
#include "opal/util/fd.h"

#include "orte/threads/threads.h"
#include "orte/mca/rml/rml_types.h"
#include "orte/mca/rmcast/rmcast_types.h"

#include "mca/pnp/pnp_types.h"
#include "mca/pnp/base/public.h"

BEGIN_C_DECLS

#define ORCM_PNP_MAX_MSGS    8

/*
 * globals that might be needed
 */
typedef struct {
    opal_list_item_t super;
    char *string_id;
    orcm_pnp_tag_t tag;
    orcm_pnp_callback_fn_t cbfunc;
    void *cbdata;
} orcm_pnp_request_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_request_t);

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
    orcm_pnp_channel_t channel;
    orte_process_name_t sender;
    opal_buffer_t buf;
} orcm_pnp_msg_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_pnp_msg_t);

/* internal base functions */
ORCM_DECLSPEC char* orcm_pnp_print_tag(orcm_pnp_tag_t tag);
ORCM_DECLSPEC char* orcm_pnp_print_channel(orcm_pnp_channel_t chan);
ORCM_DECLSPEC int orcm_pnp_base_start_threads(void);
ORCM_DECLSPEC void orcm_pnp_base_stop_threads(void);
ORCM_DECLSPEC int orcm_pnp_base_record_recv(orcm_triplet_t *triplet,
                                            orcm_pnp_channel_t channel,
                                            orcm_pnp_tag_t tag,
                                            orcm_pnp_callback_fn_t cbfunc,
                                            void *cbdata);
ORCM_DECLSPEC void orcm_pnp_base_update_pending_recvs(orcm_triplet_t *trp);
ORCM_DECLSPEC void orcm_pnp_base_check_pending_recvs(orcm_triplet_t *trp,
                                                     orcm_triplet_group_t *grp);
ORCM_DECLSPEC void orcm_pnp_base_check_trip_recvs(char *stringid,
                                                  opal_list_t *recvs,
                                                  orcm_pnp_channel_t channel);
ORCM_DECLSPEC orcm_pnp_request_t* orcm_pnp_base_find_request(opal_list_t *list,
                                                             char *string_id,
                                                             orcm_pnp_tag_t tag);
ORCM_DECLSPEC int orcm_pnp_base_pack_announcement(opal_buffer_t *buf,
                                                  orte_process_name_t *sender);
ORCM_DECLSPEC void orcm_pnp_base_process_announcements(orte_process_name_t *sender,
                                                       opal_buffer_t *buf);

ORCM_DECLSPEC void orcm_pnp_base_recv_input_buffers(int status,
                                                    orte_rmcast_channel_t channel,
                                                    orte_rmcast_seq_t seq_num,
                                                    orte_rmcast_tag_t tag,
                                                    orte_process_name_t *sender,
                                                    opal_buffer_t *buf, void *cbdata);
ORCM_DECLSPEC void orcm_pnp_base_recv_direct_msgs(int status, orte_process_name_t* sender,
                                                  opal_buffer_t* buffer, orte_rml_tag_t tg,
                                                  void* cbdata);


#define ORCM_PNP_MESSAGE_EVENT(sndr, chn, bf)                   \
    do {                                                        \
        orcm_pnp_msg_t *msg;                                    \
        OPAL_OUTPUT_VERBOSE((1, orte_debug_output,              \
                             "defining pnp msg event: %s %d",   \
                             __FILE__, __LINE__));              \
        msg = OBJ_NEW(orcm_pnp_msg_t);                          \
        msg->channel = (chn);                                   \
        msg->sender.jobid = (sndr)->jobid;                      \
        msg->sender.vpid = (sndr)->vpid;                        \
        opal_dss.copy_payload(&msg->buf, (bf));                 \
        opal_fd_write(orcm_pnp_base.recv_pipe[1],               \
                      sizeof(orcm_pnp_msg_t*), &msg);           \
    } while(0);


END_C_DECLS

#endif

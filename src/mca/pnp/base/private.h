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

/* we have to copy the data across here because
 * the underlying delivery mechanism "owns" the memory and will release
 * it once we return. Thus, we must do the copy to ensure that we "own"
 * the memory that is subsequently passed back to our caller
 */
#define ORCM_PROCESS_PNP_IOVECS(rlist, lck, cond, gp, sndr,                         \
                                chan, tg, mg, cnt, cbd)                             \
    do {                                                                            \
        int i;                                                                      \
        struct iovec *m;                                                            \
        orcm_pnp_recv_t *pkt;                                                       \
        pkt = OBJ_NEW(orcm_pnp_recv_t);                                             \
        pkt->grp = (gp);                                                            \
        pkt->src = (sndr);                                                          \
        pkt->channel = (chan);                                                      \
        pkt->tag = (tg);                                                            \
        if (NULL != (mg)) {                                                         \
            m = mg;                                                                 \
            pkt->msg = (struct iovec*)malloc((cnt)*sizeof(struct iovec));           \
            for (i=0; i < (cnt); i++) {                                             \
                pkt->msg[i].iov_len = m->iov_len;                                   \
                pkt->msg[i].iov_base = (void*)malloc(m->iov_len);                   \
                memcpy((char*)pkt->msg[i].iov_base, (char*)m->iov_base, m->iov_len);\
                m++;                                                                \
            }                                                                       \
        }                                                                           \
        pkt->count = (cnt);                                                         \
        pkt->cbdata = (cbd);                                                        \
        OPAL_THREAD_LOCK((lck));                                                    \
        opal_list_append((rlist), &pkt->super);                                     \
        opal_condition_broadcast((cond));                                           \
        OPAL_THREAD_UNLOCK((lck));                                                  \
    } while(0);

#define ORCM_PROCESS_PNP_BUFFERS(rlist, lck, cond, gp, sndr,    \
                                 chan, tg, buf, cbd)            \
    do {                                                        \
        orcm_pnp_recv_t *pkt;                                   \
        pkt = OBJ_NEW(orcm_pnp_recv_t);                         \
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

ORCM_DECLSPEC void orcm_pnp_base_push_data(orcm_pnp_source_t *src, opal_buffer_t *buf);
ORCM_DECLSPEC opal_buffer_t* orcm_pnp_base_pop_data(orcm_pnp_source_t *src);

END_C_DECLS

#endif

/*
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config_private.h"
#include "include/constants.h"

#include <stdio.h>

#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"
#include "opal/util/fd.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"
#include "orte/threads/threads.h"

#include "util/triplets.h"
#include "mca/leader/leader.h"
#include "mca/pnp/base/public.h"
#include "mca/pnp/base/private.h"

static void process_msg(orcm_pnp_msg_t *msg);
static void* rcv_processing_thread(opal_object_t *obj);
static int extract_hdr(opal_buffer_t *buf,
                       orte_process_name_t *name,
                       orte_rmcast_channel_t *channel,
                       orte_rmcast_tag_t *tag,
                       bool *restart,
                       orte_rmcast_seq_t *seq_num);

int orcm_pnp_base_start_threads(void)
{
    int rc;

    if (!orcm_pnp_base.recv_process_ctl.running) {
        OPAL_OUTPUT_VERBOSE((5, orcm_pnp_base.output,
                             "%s rmcast:base: starting recv processing thread",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        /* setup a pipe that we will use to signal the thread that a message
         * is waiting to be processed - don't define an event for it
         */
        if (pipe(orcm_pnp_base.recv_pipe) < 0) {
            opal_output(0, "%s Cannot open recv processing thread ctl pipe",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            return ORTE_ERR_OUT_OF_RESOURCE;
        }
        /* start the thread - we will send it a NULL msg pointer when
         * we want it to stop
         */
        orcm_pnp_base.recv_process.t_run = rcv_processing_thread;
        if (ORTE_SUCCESS != (rc = opal_thread_start(&orcm_pnp_base.recv_process))) {
            ORTE_ERROR_LOG(rc);
            orcm_pnp_base.recv_process_ctl.running = false;
            return rc;
        }

        OPAL_OUTPUT_VERBOSE((5, orcm_pnp_base.output,
                             "%s rmcast:base: recv processing thread started",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    }

    return ORTE_SUCCESS;
}

void orcm_pnp_base_stop_threads(void)
{
    orcm_pnp_msg_t *msg=NULL;

    OPAL_OUTPUT_VERBOSE((5, orcm_pnp_base.output,
                         "%s rmcast:base: stopping recv processing thread",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    ORTE_ACQUIRE_THREAD(&orcm_pnp_base.recv_process_ctl);
    if (orcm_pnp_base.recv_process_ctl.running) {
        ORTE_RELEASE_THREAD(&orcm_pnp_base.recv_process_ctl);
        opal_fd_write(orcm_pnp_base.recv_pipe[1], sizeof(orcm_pnp_msg_t*), &msg);
        opal_thread_join(&orcm_pnp_base.recv_process, NULL);
        ORTE_ACQUIRE_THREAD(&orcm_pnp_base.recv_process_ctl);
    }
    ORTE_RELEASE_THREAD(&orcm_pnp_base.recv_process_ctl);

    OPAL_OUTPUT_VERBOSE((5, orcm_pnp_base.output,
                         "%s rmcast:base: all threads stopped",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
}


static void* rcv_processing_thread(opal_object_t *obj)
{
    orcm_pnp_msg_t *msg;
    int rc;
    struct timespec tp={0, 10};

    OPAL_OUTPUT_VERBOSE((5, orcm_pnp_base.output,
                         "%s pnp:base: recv processing thread operational",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));

    ORTE_ACQUIRE_THREAD(&orcm_pnp_base.recv_process_ctl);
    orcm_pnp_base.recv_process_ctl.running = true;
    ORTE_RELEASE_THREAD(&orcm_pnp_base.recv_process_ctl);

    while (1) {
        /* block here until a trigger arrives */
        if (0 > (rc = opal_fd_read(orcm_pnp_base.recv_pipe[0],
                                   sizeof(orcm_pnp_msg_t*), &msg))) {
            /* if something bad happened, punt */
            opal_output(0, "%s PUNTING THREAD", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            ORTE_ACQUIRE_THREAD(&orcm_pnp_base.recv_process_ctl);
            orcm_pnp_base.recv_process_ctl.running = false;
            ORTE_RELEASE_THREAD(&orcm_pnp_base.recv_process_ctl);
            /* give a little delay to ensure the main thread gets into
             * opal_thread_join before we exit
             */
            nanosleep(&tp, NULL);
            return OPAL_THREAD_CANCELLED;
        }
        /* check to see if we were told to stop */
        if (NULL == msg) {
            ORTE_ACQUIRE_THREAD(&orcm_pnp_base.recv_process_ctl);
            orcm_pnp_base.recv_process_ctl.running = false;
            ORTE_RELEASE_THREAD(&orcm_pnp_base.recv_process_ctl);
            return OPAL_THREAD_CANCELLED;
        }

        /* process it - processing function releases the msg */
        process_msg(msg);
    }
}

static void process_msg(orcm_pnp_msg_t *msg)
{
    int n, rc;
    int8_t flag;
    int32_t i, num_iovecs, num_bytes;
    struct iovec *iovecs=NULL;
    orcm_pnp_tag_t tag;
    char *string_id=NULL;
    orcm_pnp_channel_obj_t *chan;
    orcm_pnp_request_t *request;
    orcm_triplet_t *trp;
    orcm_source_t *src;

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s Processing message from %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(&msg->sender)));

    /* extract the string id of the sender's triplet */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(&msg->buf, &string_id, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        goto DEPART;
    }    
    
    /* extract the pnp tag */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(&msg->buf, &tag, &n, ORCM_PNP_TAG_T))) {
        ORTE_ERROR_LOG(rc);
        goto DEPART;
    }

    /* if this is an announcement, process it immediately - do not
     * push it onto the recv thread! Otherwise, any immediate msgs
     * sent by that proc can be lost due to a race condition
     */
    if (ORCM_PNP_TAG_ANNOUNCE == tag) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s Processing announcement from %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&msg->sender)));

        free(string_id);
        /* unpack the iovec vs buffer flag to maintain place in buffer */
        n=1;
        if (ORCM_SUCCESS != (rc = opal_dss.unpack(&msg->buf, &flag, &n, OPAL_INT8))) {
            ORTE_ERROR_LOG(rc);
            goto DEPART;
        }
        orcm_pnp_base_process_announcements(&msg->sender, &msg->buf);
        goto DEPART;
    }

    /* if this is coming on our direct channel, then always listen to
     * it. Otherwise, check to see if the message is from the leader of
     * this triplet
     */
    if (ORCM_PNP_DIRECT_CHANNEL == msg->channel) {
        /* check to see if the sender is still alive - we may
         * have received notification of death while waiting
         * for this message to be processed
         */
        if (NULL == (trp = orcm_get_triplet_stringid(string_id))) {
            goto DEPART;
        }
        if (NULL == (src = orcm_get_source(trp, &msg->sender, false))) {
            ORTE_RELEASE_THREAD(&trp->ctl);
            goto DEPART;
        }
        if (!src->alive) {
            ORTE_RELEASE_THREAD(&src->ctl);
            ORTE_RELEASE_THREAD(&trp->ctl);
            goto DEPART;
        }
        ORTE_RELEASE_THREAD(&src->ctl);
        ORTE_RELEASE_THREAD(&trp->ctl);
    } else {
        if (!orcm_leader.deliver_msg(string_id, &msg->sender)) {
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s Message from %s of triplet %s ignored - not leader",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(&msg->sender), string_id));
            goto DEPART;
        }
    }

    /* deal with alias */
    if (ORCM_PNP_DIRECT_CHANNEL == msg->channel) {
        msg->channel = orcm_pnp_base.my_input_channel->channel;
    }

    /* get the channel object */
    if (NULL == (chan = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&orcm_pnp_base.channels, msg->channel))) {
        /* unrecognized channel - ignore message */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s Unrecognized channel %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             orcm_pnp_print_channel(msg->channel)));
        goto DEPART;
    }

    /* find the request object for this tag */
    if (NULL == (request = orcm_pnp_base_find_request(&chan->recvs, string_id, tag))) {
        /* no matching requests */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:recv triplet %s has no matching recvs for channel %s tag %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             string_id, orcm_pnp_print_channel(msg->channel), orcm_pnp_print_tag(tag)));
        goto DEPART;
    }

    /* unpack the iovec vs buffer flag */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(&msg->buf, &flag, &n, OPAL_INT8))) {
        ORTE_ERROR_LOG(rc);
        goto DEPART;
    }    
    
    if (0 == flag) {
        /* iovecs were sent - get them */
        n=1;
        if (ORCM_SUCCESS != (rc = opal_dss.unpack(&msg->buf, &num_iovecs, &n, OPAL_INT32))) {
            ORTE_ERROR_LOG(rc);
            goto DEPART;
        }
        if (0 < num_iovecs) {
            iovecs = (struct iovec *)malloc(num_iovecs * sizeof(struct iovec));
            for (i=0; i < num_iovecs; i++) {
                n=1;
                if (ORCM_SUCCESS != (rc = opal_dss.unpack(&msg->buf, &num_bytes, &n, OPAL_INT32))) {
                    ORTE_ERROR_LOG(rc);
                    goto DEPART;
                }
                iovecs[i].iov_len = num_bytes;
                if (0 < num_bytes) {
                    iovecs[i].iov_base = (uint8_t*)malloc(num_bytes);
                    if (ORCM_SUCCESS != (rc = opal_dss.unpack(&msg->buf, iovecs[i].iov_base, &num_bytes, OPAL_UINT8))) {
                        ORTE_ERROR_LOG(rc);
                        goto DEPART;
                    }
                } else {
                    iovecs[i].iov_base = NULL;
                }
            }
        }
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received input iovecs - delivering msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));

        request->cbfunc(ORCM_SUCCESS, &msg->sender, tag, iovecs, num_iovecs, NULL, request->cbdata);
        /* release the memory */
        if (0 < num_iovecs) {
            for (i=0; i < num_iovecs; i++) {
                free(iovecs[i].iov_base);
            }
            free(iovecs);
        }
        goto DEPART;
    }

    if (1 == flag) {
        /* buffer was sent - just hand it over */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received input buffer - delivering msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        request->cbfunc(ORCM_SUCCESS, &msg->sender, tag, NULL, 0, &msg->buf, request->cbdata);
    }
 DEPART:
    OBJ_RELEASE(msg);
}

void orcm_pnp_base_recv_input_buffers(int status,
                                      orte_rmcast_channel_t channel,
                                      orte_rmcast_seq_t seq_num,
                                      orte_rmcast_tag_t tag,
                                      orte_process_name_t *sender,
                                      opal_buffer_t *buf, void *cbdata)
{
    /* if we have not announced, ignore this message */
    if (NULL == orcm_pnp_base.my_string_id || !orcm_pnp_base.comm_enabled) {
        return;
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:base:received input buffer on channel %s seq %d tag %s from sender %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), orcm_pnp_print_channel(channel), seq_num,
                         orcm_pnp_print_tag(tag), ORTE_NAME_PRINT(sender)));
    
    /* if this message is from myself, ignore it */
    if (sender->jobid == ORTE_PROC_MY_NAME->jobid &&
        sender->vpid == ORTE_PROC_MY_NAME->vpid) {
        return;
    }

    /* schedule for delivery */
    ORCM_PNP_MESSAGE_EVENT(sender, channel, buf);
}

void orcm_pnp_base_recv_direct_msgs(int status, orte_process_name_t* sender,
                                    opal_buffer_t* buffer, orte_rml_tag_t tg,
                                    void* cbdata)
{
    /* if we have not announced, ignore this message */
    if (NULL == orcm_pnp_base.my_string_id || !orcm_pnp_base.comm_enabled) {
        return;
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:base recvd direct msg from %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));
    
    /* schedule for delivery */
    ORCM_PNP_MESSAGE_EVENT(sender, ORCM_PNP_DIRECT_CHANNEL, buffer);
}


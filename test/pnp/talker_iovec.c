/* -*- C -*-
 *
 * $HEADER$
 *
 * A program that generates output to be consumed
 * by a pnp listener
 */
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "opal/dss/dss.h"
#include "opal/event/event.h"
#include "opal/util/output.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"

#include "util/triplets.h"

#include "mca/pnp/pnp.h"
#include "runtime/runtime.h"

static void send_data(int fd, short flags, void *arg);
static int msg_num=0, block=0;
static orte_process_name_t listener;
static bool found_listener = false;
static orcm_pnp_channel_t peer = ORCM_PNP_INVALID_CHANNEL;
static struct timeval tp;
static bool clear_to_send=false;

#define ORCM_TEST_CLIENT_SERVER_TAG     110

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buf,
                       void *cbdata);

static void found_channel(const char *app,
                          const char *version,
                          const char *release,
                          orcm_pnp_channel_t channel);

int main(int argc, char* argv[])
{
    int rc;
    
    if (1 < argc) {
        tp.tv_sec = strtol(argv[1], NULL, 10);
    } else {
        tp.tv_sec = 0;
    }
    if (2 < argc) {
        tp.tv_usec = strtol(argv[2], NULL, 10);
    } else {
        tp.tv_usec = 10000;
    }

    /* init the ORCM library - this includes registering
     * a multicast recv so we hear announcements and
     * their responses from other apps
     */
    if (ORCM_SUCCESS != (rc = orcm_init(ORCM_APP))) {
        fprintf(stderr, "Failed to init: error %d\n", rc);
        exit(1);
    }
    
    /* open a channel to send to the listener application */
    if (ORCM_SUCCESS != (rc = orcm_pnp.open_channel("LISTENER_IOVEC", "1.0", "alpha",
                                                    ORTE_JOBID_WILDCARD, found_channel))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* listen on my input channel for direct messages */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_receive("TALKER_IOVEC", "1.0", "alpha",
                                                        ORCM_PNP_GROUP_INPUT_CHANNEL,
                                                        ORCM_PNP_TAG_WILDCARD, recv_input, NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* announce our existence */
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("TALKER_IOVEC", "1.0", "alpha", NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* wake up every x seconds to send something */
    ORTE_TIMER_EVENT(tp.tv_sec, tp.tv_usec, send_data);

    opal_event_dispatch();
    
cleanup:
    orcm_finalize();
    return rc;
}

static void cbfunc(int status, orte_process_name_t *name,
                   orcm_pnp_tag_t tag,
                   struct iovec *msg, int count,
                   opal_buffer_t *buf, void *cbdata)
{
    int j;

    for (j=0; j < count; j++) {
        if (NULL != msg[j].iov_base) {
            free(msg[j].iov_base);
        }
    }
    free(msg);
}

static void send_data(int fd, short flags, void *arg)
{
    int32_t count=5, *ptr;
    int rc;
    int i, j, n;
    struct iovec *msg;
    opal_event_t *tmp = (opal_event_t*)arg;

    if (!clear_to_send) {
        goto reset;
    }

    block++;
    for (i=0; i < 5; i++) {
        msg = (struct iovec*)malloc(count * sizeof(struct iovec));
        for (j=0; j < count; j++) {
            msg[j].iov_base = (void*)malloc(count * sizeof(int32_t));
            ptr = msg[j].iov_base;
            for (n=0; n < count; n++) {
                *ptr = msg_num;
                ptr++;
            }
            msg[j].iov_len = count * sizeof(int32_t);
        }
    
        /* output the values */
        if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(peer, &listener,
                                                     ORCM_TEST_CLIENT_SERVER_TAG, msg, count,
                                                     NULL, cbfunc, NULL))) {
            ORTE_ERROR_LOG(rc);
        }

        msg_num++;
    }
    /* do not send another block until ack recvd */
    clear_to_send = false;

    if (0 == (block % 100)) {
        opal_output(0, "%s sent direct msg block %d to %s",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    block, ORTE_NAME_PRINT(&listener));
    }

 reset:
    /* reset the timer */
    opal_evtimer_add(tmp, &tp);
}

static void found_channel(const char *app, const char *version,
                          const char *release,
                          orcm_pnp_channel_t channel)
{

    opal_output(0, "%s FOUND CHANNEL %d for app %s:%s:%s",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)channel, app, version, release);
    peer = channel;

    if (ORCM_SUCCESS != orcm_triplet_get_process(app, version, release, &listener)) {
        /* something is wrong */
        opal_output(0, "%s could not get any process name within %s:%s:%s",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), app, version, release);
        exit(1);
    }
    clear_to_send = true;
}

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buf,
                       void *cbdata)
{
    if (0 == (block % 100)) {
        opal_output(0, "%s ACK RECVD FOR BLOCK %d", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), block);
    }
    /* when we recv something, it is by default an ack - so send more data */
    clear_to_send = true;
}

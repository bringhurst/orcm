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
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#include "opal/dss/dss.h"
#include "opal/event/event.h"
#include "opal/util/output.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"

#include "mca/pnp/pnp.h"
#include "runtime/runtime.h"

#define ORCM_TEST_CLIENT_SERVER_TAG     110
#define ORCM_TEST_CLIENT_CLIENT_TAG     120

static void send_data(int fd, short flags, void *arg);
static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buffer,
                       void *cbdata);
static void server_output(int status,
                          orte_process_name_t *sender,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg, int count,
                          opal_buffer_t *buffer,
                          void *cbdata);
static void found_channel(char *app, char *version, char *release,
                          orcm_pnp_channel_t channel);

static int32_t flag=0;
static int msg_num=0;
static orcm_pnp_channel_t peer = ORCM_PNP_INVALID_CHANNEL;

int main(int argc, char* argv[])
{
    struct timespec tp;
    int rc;

    /* init the ORCM library - this includes registering
     * a multicast recv so we hear announcements and
     * their responses from other apps
     */
    if (ORCM_SUCCESS != (rc = orcm_init(ORCM_APP))) {
        fprintf(stderr, "Failed to init: error %d\n", rc);
        exit(1);
    }
    
    /* open a channel to send to the server application */
    if (ORCM_SUCCESS != (rc = orcm_pnp.open_channel("SERVER", "1.0", "alpha", found_channel))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* register to recv anything sent to our input  */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_receive("client", "1.0", "alpha",
                                                        ORCM_PNP_GROUP_INPUT_CHANNEL,
                                                        ORCM_PNP_TAG_WILDCARD, recv_input))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* register to recv anything the server outputs */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_receive("SERVER", "1.0", "alpha",
                                                        ORCM_PNP_GROUP_OUTPUT_CHANNEL,
                                                        ORCM_PNP_TAG_WILDCARD, server_output))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* announce our existence */
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("CLIENT", "1.0", "alpha", NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* wake up every x seconds to send something */
    ORTE_TIMER_EVENT(2, 0, send_data);
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
    int i;
    
    for (i=0; i < count; i++) {
        if (NULL != msg[i].iov_base) {
            free(msg[i].iov_base);
        }
    }
    free(msg);
}
static void send_data(int fd, short flags, void *arg)
{
    int32_t count, *ptr;
    int rc;
    int j, n;
    struct iovec *msg;
    opal_event_t *tmp = (opal_event_t*)arg;
    struct timeval now;

    count = ORTE_PROC_MY_NAME->vpid+1;
    msg = (struct iovec*)malloc(count * sizeof(struct iovec));
    for (j=0; j < count; j++) {
        msg[j].iov_base = (void*)malloc(5 * sizeof(int32_t));
        ptr = msg[j].iov_base;
        for (n=0; n < 5; n++) {
            *ptr = msg_num;
            ptr++;
        }
        msg[j].iov_len = 5 * sizeof(int32_t);
    }
    
    /* output the values */
    if (ORCM_PNP_INVALID_CHANNEL == peer || msg_num % 2) {
        opal_output(0, "%s mcasting data on output channel for msg number %d",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), msg_num);
        if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_GROUP_OUTPUT_CHANNEL, NULL,
                                                     ORCM_PNP_TAG_OUTPUT, msg, count, NULL, cbfunc, NULL))) {
            ORTE_ERROR_LOG(rc);
        }
    } else {
        opal_output(0, "%s mcasting data on channel %d for msg number %d",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), peer, msg_num);
        if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_GROUP_OUTPUT_CHANNEL, NULL,
                                                     ORCM_PNP_TAG_OUTPUT, msg, count, NULL, cbfunc, NULL))) {
            ORTE_ERROR_LOG(rc);
        }
    }
    
    /* increment the msg number */
    msg_num++;
    
    /* reset the timer */
    now.tv_sec = 2;
    now.tv_usec = 0;
    opal_evtimer_add(tmp, &now);
    
}

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buffer,
                       void *cbdata)
{
    int32_t i, n, *data;

    opal_output(0, "%s recvd message on my input from %s on tag %d with %d iovecs",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                ORTE_NAME_PRINT(sender), (int)tag, count);

    /* loop over the iovecs */
    for (i=0; i < count; i++) {
        /* check the number of values */
        if (20 != msg[i].iov_len) {
            opal_output(0, "\tError: iovec has incorrect length %d", (int)msg[i].iov_len);
            return;
        }
        
        /* print the first value */
        data = (int32_t*)msg[i].iov_base;
        opal_output(0, "\tValue in first posn: %d", data[0]);
        
        /* now check the values */
        for (n=1; n < 5; n++) {
            if (data[n] != data[0]) {
                opal_output(0, "\tError: invalid data %d at posn %d", data[n], n);
                return;
            }
        }
   }
}

static void server_output(int status,
                          orte_process_name_t *sender,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg, int count,
                          opal_buffer_t *buffer,
                          void *cbdata)
{
    int32_t i, n, *data;

    opal_output(0, "%s recvd output from %s on tag %d with %d iovecs",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                ORTE_NAME_PRINT(sender), (int)tag, count);

    /* loop over the iovecs */
    for (i=0; i < count; i++) {
        /* check the number of values */
        if (20 != msg[i].iov_len) {
            opal_output(0, "\tError: iovec has incorrect length %d", (int)msg[i].iov_len);
            return;
        }
        
        /* print the first value */
        data = (int32_t*)msg[i].iov_base;
        opal_output(0, "\tValue in first posn: %d", data[0]);
        
        /* now check the values */
        for (n=1; n < 5; n++) {
            if (data[n] != data[0]) {
                opal_output(0, "\tError: invalid data %d at posn %d", data[n], n);
                return;
            }
        }
   }
    
}

static void found_channel(char *app, char *version, char *release,
                          orcm_pnp_channel_t channel)
{
    opal_output(0, "%s recvd channel %d for triplet %s:%s:%s",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                channel, app, version, release);
    peer = channel;
}

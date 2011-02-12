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
static void found_channel(const char *app,
                          const char *version,
                          const char *release,
                          orcm_pnp_channel_t channel);

static int32_t flag=0;
static int msg_num=0;
static orcm_pnp_channel_t peer = ORCM_PNP_INVALID_CHANNEL;
static struct timeval tp;
static int report_rate;

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

    if (tp.tv_usec <= 100) {
        report_rate = 10000;
    } else if (tp.tv_usec <= 1000) {
        report_rate = 1000;
    } else {
        report_rate = 100;
    }

    /* init the ORCM library - this includes registering
     * a multicast recv so we hear announcements and
     * their responses from other apps
     */
    if (ORCM_SUCCESS != (rc = orcm_init(ORCM_APP))) {
        fprintf(stderr, "Failed to init: error %d\n", rc);
        exit(1);
    }
    
    /* open a channel to send to the server application */
    if (ORCM_SUCCESS != (rc = orcm_pnp.open_channel("SERVER", "1.0", "alpha", ORTE_JOBID_WILDCARD, found_channel))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* register to recv anything sent to our input  */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_receive("client", "1.0", "alpha",
                                                        ORCM_PNP_GROUP_INPUT_CHANNEL,
                                                        ORCM_PNP_TAG_WILDCARD, recv_input, NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* register to recv anything the server outputs */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_receive("SERVER", "1.0", "alpha",
                                                        ORCM_PNP_GROUP_OUTPUT_CHANNEL,
                                                        ORCM_PNP_TAG_WILDCARD, server_output, NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* announce our existence */
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("CLIENT", "1.0", "alpha", NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    opal_output(0, "CLIENT %s ACTIVE", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));

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
    
    if (0 == (msg_num % report_rate)) {
        opal_output(0, "%s mcasting data for msg number %d",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), msg_num);
    }

    /* output the values */
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_GROUP_OUTPUT_CHANNEL, NULL,
                                                 ORCM_PNP_TAG_OUTPUT, msg, count, NULL, cbfunc, NULL))) {
        ORTE_ERROR_LOG(rc);
        for (j=0; j < count; j++) {
            if (NULL != msg[j].iov_base) {
                free(msg[j].iov_base);
            }
        }
        free(msg);
    }
    
    /* increment the msg number */
    msg_num++;
    
    /* reset the timer */
    opal_evtimer_add(tmp, &tp);
    
}

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buffer,
                       void *cbdata)
{
    int32_t i, n, *data;

    /* loop over the iovecs */
    for (i=0; i < count; i++) {
        /* check the number of values */
        if (20 != msg[i].iov_len) {
            opal_output(0, "\tError: iovec has incorrect length %d", (int)msg[i].iov_len);
            return;
        }
        data = (int32_t*)msg[i].iov_base;
        /* now check the values */
        for (n=1; n < 5; n++) {
            if (data[n] != data[0]) {
                opal_output(0, "\tError: invalid data %d at iovec %d posn %d", data[n], i, n);
                return;
            }
        }
    }
    opal_output(0, "%s recvd direct msg %d from sender %s",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                data[0], ORTE_NAME_PRINT(sender));
}

static void server_output(int status,
                          orte_process_name_t *sender,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg, int count,
                          opal_buffer_t *buffer,
                          void *cbdata)
{
    int32_t i, n, *data;

    /* loop over the iovecs */
    for (i=0; i < count; i++) {
        /* check the number of values */
        if (20 != msg[i].iov_len) {
            opal_output(0, "\tError: iovec has incorrect length %d", (int)msg[i].iov_len);
            return;
        }
        data = (int32_t*)msg[i].iov_base;
        /* now check the values */
        for (n=1; n < 5; n++) {
            if (data[n] != data[0]) {
                opal_output(0, "\tError: invalid data %d at posn %d", data[n], n);
                return;
            }
        }
   }
    if (0 == (data[0] % report_rate)) {
        opal_output(0, "%s recvd mcast data sender %s msg number %d",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    ORTE_NAME_PRINT(sender), data[0]);
    }
}

static void found_channel(const char *app, const char *version,
                          const char *release,
                          orcm_pnp_channel_t channel)
{
    opal_output(0, "%s FOUND CHANNEL %d for app %s:%s:%s",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)channel, app, version, release);
    peer = channel;
}

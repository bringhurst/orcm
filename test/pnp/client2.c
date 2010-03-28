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

#define ORCM_TEST_CLIENT_SERVER_TAG     15
#define ORCM_TEST_CLIENT_CLIENT_TAG     16

static void send_data(int fd, short flags, void *arg);
static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       void *cbdata);

static int32_t flag=0;
static int msg_num;
static orcm_pnp_channel_t peer;

int main(int argc, char* argv[])
{
    struct timespec tp;
    int rc, delay;

    /* init the ORCM library - this includes registering
     * a multicast recv so we hear announcements and
     * their responses from other apps
     */
    if (ORCM_SUCCESS != (rc = orcm_init(OPENRCM_APP))) {
        fprintf(stderr, "Failed to init: error %d\n", rc);
        exit(1);
    }
    
    /* announce our existence */
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("CLIENT", "2.0", "beta", NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* for this application, register an input to hear direct responses */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_input("SERVER", "1.0", "alpha",
                                                      ORCM_PNP_GROUP_OUTPUT_CHANNEL,
                                                      ORCM_TEST_CLIENT_SERVER_TAG, recv_input))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* open a channel to any client 1.0 peers */
    if (ORCM_PNP_INVALID_CHANNEL == (peer = orcm_pnp.open_channel("client", "1.0", NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* init the msg number */
    msg_num = 0;
    
    /* wake up every delay microseconds and send something */
    delay = (ORTE_PROC_MY_NAME->vpid + 1);
    opal_output(0, "sending data every %d seconds", delay);
    tp.tv_sec = delay;
    tp.tv_nsec = 0;
    while (1) {
        nanosleep(&tp, NULL);
        send_data(0, 0, NULL);
    }

cleanup:
    orcm_finalize();
    return rc;
}

static void cbfunc(int status, orte_process_name_t *name, orcm_pnp_tag_t tag,
                   struct iovec *msg, int count, void *cbdata)
{
    int i;
    
    opal_output(0, "%s mesg sent", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
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
    opal_output(0, "%s sending data for msg number %d", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), msg_num);
#if 0
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_GROUP_OUTPUT_CHANNEL, NULL,
                                                 ORCM_PNP_TAG_OUTPUT, msg, count, NULL, NULL))) {
        ORTE_ERROR_LOG(rc);
    }
#endif
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(peer, NULL,
                                                 ORCM_TEST_CLIENT_CLIENT_TAG, msg, count, cbfunc, NULL))) {
        ORTE_ERROR_LOG(rc);
    }
    
    /* increment the msg number */
    msg_num++;
}

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       void *cbdata)
{
    opal_output(0, "%s recvd message from %s on tag %d",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                ORTE_NAME_PRINT(sender), (int)tag);
}


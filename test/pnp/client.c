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
#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif  /*  HAVE_SIGNAL_H */
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
#include "orte/runtime/orte_wait.h"

#include "mca/pnp/pnp.h"
#include "runtime/runtime.h"

#define ORCM_TEST_CLIENT_SERVER_TAG     12345

static struct opal_event term_handler;
static struct opal_event int_handler;
static void abort_exit_callback(int fd, short flags, void *arg);
static void send_data(int fd, short flags, void *arg);
static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       opal_buffer_t *buf,
                       void *cbdata);

static int32_t flag=0;
static int msg_num, delay;

int main(int argc, char* argv[])
{
    int rc;
    
    /* init the ORCM library - this includes registering
     * a multicast recv so we hear announcements and
     * their responses from other apps
     */
    if (ORCM_SUCCESS != (rc = orcm_init(OPENRCM_APP))) {
        fprintf(stderr, "Failed to init: error %d\n", rc);
        exit(1);
    }
    
    /** setup callbacks for abort signals - from this point
     * forward, we need to abort in a manner that allows us
     * to cleanup
     */
    opal_signal_set(&term_handler, SIGTERM,
                    abort_exit_callback, &term_handler);
    opal_signal_add(&term_handler, NULL);
    opal_signal_set(&int_handler, SIGINT,
                    abort_exit_callback, &int_handler);
    opal_signal_add(&int_handler, NULL);
    
    /* announce our existence */
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("CLIENT", "1.0", "alpha"))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* for this application, register an input to hear direct responses */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_input_buffer("SERVER", "1.0", "alpha",
                                                             ORCM_TEST_CLIENT_SERVER_TAG, recv_input))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* compute a wait time */
    delay = 200*(ORTE_PROC_MY_NAME->vpid + 1);
    opal_output(0, "sending data every %d microseconds", delay);
    
    /* init the msg number */
    msg_num = 0;
    
    /* wake up every delay microseconds and send something */
    ORTE_TIMER_EVENT(1, delay, send_data);
    
    /* just sit here */
    opal_event_dispatch();

cleanup:
    orcm_finalize();
    return rc;
}

static void send_data(int fd, short flags, void *arg)
{
    int32_t myval;
    int rc;
    int j;
    float randval;
    opal_event_t *tmp = (opal_event_t*)arg;
    struct timeval now;
    struct iovec msg[2];
    int count;
    int32_t data[5];

    /* prep the message */
    data[0] = msg_num;
    data[1] = data[2] = data[3] = data[4] = data[0];
    msg[0].iov_base = (void*)data;
    msg[0].iov_len = 20;
    msg[1].iov_base = (void*)data;
    msg[1].iov_len = 20;
    
    /* output the values */
    opal_output(0, "%s sending data for msg number %d", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), msg_num);
    if (0 == ORTE_PROC_MY_NAME->vpid) {
        count = 1;
    } else {
        count = 2;
    }
    if (ORCM_SUCCESS != (rc = orcm_pnp.output(NULL, ORCM_PNP_TAG_OUTPUT, msg, count))) {
        ORTE_ERROR_LOG(rc);
    }

    /* increment the msg number */
    msg_num++;

    /* reset the timer */
    now.tv_sec = 1;
    now.tv_usec = delay;
    opal_evtimer_add(tmp, &now);
}

static void abort_exit_callback(int fd, short ign, void *arg)
{
    int j;
    orte_job_t *jdata;
    opal_list_item_t *item;
    int ret;
    
    /* Remove the TERM and INT signal handlers */
    opal_signal_del(&term_handler);
    opal_signal_del(&int_handler);
    
    orcm_finalize();
    exit(1);
}

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       opal_buffer_t *buf,
                       void *cbdata)
{
    opal_output(0, "%s recvd message from server %s on tag %d",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                ORTE_NAME_PRINT(sender), (int)tag);
    
}


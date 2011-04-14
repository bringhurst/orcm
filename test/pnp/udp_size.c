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
#include "opal/mca/event/event.h"
#include "opal/util/output.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"

#include "util/triplets.h"

#include "mca/pnp/pnp.h"
#include "runtime/runtime.h"

static void signal_trap(int signal, short flags, void *arg)
{
    /* cannot directly call finalize and exit as
     * we are in a signal handler - and the OS
     * would be extremely upset with us!
     */
    orte_abnormal_term_ordered = true;
    ORTE_UPDATE_EXIT_STATUS(128+signal);
    ORTE_TIMER_EVENT(0, 0, orcm_just_quit);
}

static void send_data(int fd, short flags, void *arg);
static int msg_num=0;
static struct timeval tp;
static opal_event_t sigterm_handler, sigint_handler;

#define ORCM_TEST_CLIENT_SERVER_TAG     110

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
    
    opal_event_signal_set(opal_event_base, &sigterm_handler, SIGTERM,
                          signal_trap, &sigterm_handler);
    opal_event_signal_add(&sigterm_handler, NULL);
    opal_event_signal_set(opal_event_base, &sigint_handler, SIGINT,
                          signal_trap, &sigint_handler);
    opal_event_signal_add(&sigint_handler, NULL);

    /* announce our existence */
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("UDP_SIZE", "1.0", "alpha", NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* wake up every x seconds to send something */
    ORTE_TIMER_EVENT(tp.tv_sec, tp.tv_usec, send_data);

    opal_event_dispatch(opal_event_base);
    
cleanup:
    orcm_finalize();
    return rc;
}

static void send_data(int fd, short flags, void *arg)
{
    int rc, count;
    struct iovec msg;
    opal_event_t *tmp = (opal_event_t*)arg;

    count = msg_num * 1024;
    msg.iov_base = (void*)malloc(count * sizeof(uint8_t));
    msg.iov_len = count * sizeof(uint8_t);
    
    /* output the values */
    if (ORCM_SUCCESS != (rc = orcm_pnp.output(ORCM_PNP_GROUP_OUTPUT_CHANNEL, NULL,
                                                 ORCM_TEST_CLIENT_SERVER_TAG, &msg, 1, NULL))) {
        opal_output(0, "%s failed to send msg size %d",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), count);
        free(msg.iov_base);
        orcm_finalize();
        exit(0);
    }
    free(msg.iov_base);
    msg_num++;


    opal_output(0, "%s sent msg size %d",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), count);

    /* reset the timer */
    opal_event_evtimer_add(tmp, &tp);
}

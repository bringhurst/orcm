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

int main(int argc, char* argv[])
{
    struct timeval tp;
    int rc;
    
    /* seed the random number generator */
    gettimeofday (&tp, NULL);
    srand (tp.tv_usec);
    
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
    
    /* wake up every 5 seconds and send something */
    ORTE_TIMER_EVENT(5, 0, send_data);
    
    /* just sit here */
    opal_event_dispatch();

cleanup:
    orcm_finalize();
    return rc;
}

static void send_data(int fd, short flags, void *arg)
{
    int32_t myval;
    opal_buffer_t buf;
    int rc;
    int j;
    float randval;
    opal_event_t *tmp = (opal_event_t*)arg;
    struct timeval now;

    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    /* pack the flag */
    opal_dss.pack(&buf, &flag, 1, OPAL_INT32);
    /* toggle the flag */
    flag++;
    if (3 < flag) {
        flag = 0;
    }
    for (j=0; j < 100; j++) {
        randval = rand();
        myval = randval * 100;
        opal_dss.pack(&buf, &myval, 1, OPAL_INT32);
    }
    /* output the values */
    opal_output(0, "%s sending data", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_buffer(NULL, ORCM_PNP_TAG_OUTPUT, &buf))) {
        ORTE_ERROR_LOG(rc);
    }
    OBJ_DESTRUCT(&buf);

    /* reset the timer */
    now.tv_sec = 5;
    now.tv_usec = 0;
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


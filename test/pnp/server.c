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

#include "mca/pnp/pnp.h"
#include "mca/leader/leader.h"
#include "runtime/runtime.h"

#define ORCM_TEST_CLIENT_SERVER_TAG     12345

static struct opal_event term_handler;
static struct opal_event int_handler;
static void abort_exit_callback(int fd, short flags, void *arg);

/* our message recv function */
static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       opal_buffer_t *buf,
                       void *cbdata);

static void ldr_failed(char *app,
                       char *version,
                       char *release,
                       int sibling);

int main(int argc, char* argv[])
{
    int i, j;
    float randval, pi;
    struct timeval tp;
    int rc;
    int32_t myval;
    opal_buffer_t buf;
    
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
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("SERVER", "1.0", "alpha"))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* we want to accept ALL input messages */
    if (ORCM_SUCCESS != (rc = orcm_leader.set_leader("CLIENT", "1.0", "alpha",
                                                     ORCM_LEADER_WILDCARD, ldr_failed))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* we want to listen to the CLIENT app */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_input_buffer("CLIENT", "1.0", "alpha",
                                                             ORCM_PNP_TAG_OUTPUT, recv_input))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* just sit here */
    opal_event_dispatch();

cleanup:
    /* Remove the TERM and INT signal handlers */
    opal_signal_del(&term_handler);
    opal_signal_del(&int_handler);

    orcm_finalize();
    return rc;
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
    int32_t i32, n;
    opal_buffer_t response;
    
    opal_output(0, "%s recvd message from client %s on tag %d",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                ORTE_VPID_PRINT(sender->vpid), (int)tag);
    
    /* unpack the first value in the recvd buffer */
    n=1;
    opal_dss.unpack(buf, &i32, &n, OPAL_INT32);
    
    /* if it is zero, then send a response */
    if (0 == i32) {
        OBJ_CONSTRUCT(&response, opal_buffer_t);
        opal_dss.pack(&response, &i32, 1, OPAL_INT32);
        orcm_pnp.output_buffer(sender, ORCM_TEST_CLIENT_SERVER_TAG, &response);
        OBJ_DESTRUCT(&response);
    }
}

static void ldr_failed(char *app,
                       char *version,
                       char *release,
                       int sibling)
{
    opal_output(0, "%s LEADER FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
}
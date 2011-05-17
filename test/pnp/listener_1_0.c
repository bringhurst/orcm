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

/* our message recv function */
static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buf,
                       void *cbdata);

static opal_event_t sigterm_handler, sigint_handler;

int main(int argc, char* argv[])
{
    int i;
    float pi;
    int rc;
    
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
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("LISTENER", "1.0", "alpha", NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* we want to listen to the TALKER app */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_receive("TALKER", "1.0", "alpha",
                                                        ORCM_PNP_GROUP_OUTPUT_CHANNEL,
                                                        ORCM_PNP_TAG_OUTPUT, recv_input, NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* just sit here */
    opal_event_dispatch(opal_event_base);

cleanup:

    orcm_finalize();
    return rc;
}

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buf,
                       void *cbdata)
{
    int rc;
    
    opal_output(0, "%s recvd message from talker %s on tag %d",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                ORTE_NAME_PRINT(sender), (int)tag);
}


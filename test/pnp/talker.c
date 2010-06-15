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

#include "mca/pnp/pnp.h"
#include "runtime/runtime.h"

static void send_data(int fd, short flags, void *arg);
static int counter=0;

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
    
    /* announce our existence */
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("TALKER", "1.0", "alpha", NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* for this application, there are no desired
     * inputs, so we don't register any
     */
    ORTE_TIMER_EVENT(ORTE_PROC_MY_NAME->vpid + 1, 0, send_data);
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
    OBJ_RELEASE(buf);
}

static void send_data(int fd, short flags, void *arg)
{
    int32_t myval;
    opal_buffer_t *buf;
    int rc;
    int j;
    float randval;
    opal_event_t *tmp = (opal_event_t*)arg;
    struct timeval now;

    buf = OBJ_NEW(opal_buffer_t);
    opal_dss.pack(buf, ORTE_PROC_MY_NAME, 1, ORTE_NAME);
    for (j=0; j < 100; j++) {
        randval = rand();
        myval = randval * 100;
        opal_dss.pack(buf, &myval, 1, OPAL_INT32);
    }
    /* output the values */
    opal_output(0, "%s sending msg number %d", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), counter);
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_GROUP_OUTPUT_CHANNEL, NULL,
                                                 ORCM_PNP_TAG_OUTPUT, NULL, 0, buf, cbfunc, NULL))) {
        ORTE_ERROR_LOG(rc);
    }
    counter++;
    
    /* reset the timer */
    now.tv_sec = ORTE_PROC_MY_NAME->vpid + 1;
    now.tv_usec = 0;
    opal_evtimer_add(tmp, &now);
}


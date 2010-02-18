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

static struct opal_event term_handler;
static struct opal_event int_handler;
static void abort_exit_callback(int fd, short flags, void *arg);
static void send_data(int fd, short flags, void *arg);
static int msg_num=0;

int main(int argc, char* argv[])
{
    struct timespec tp;
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
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("TALKER_IOVEC", "1.0", "alpha", NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* for this application, there are no desired
     * inputs, so we don't register any
     */
    tp.tv_sec = 0;
    tp.tv_nsec = 100000000*(2*ORTE_PROC_MY_NAME->vpid + 1);
    while (1) {
        nanosleep(&tp, NULL);
        send_data(0, 0, NULL);
    }
    
cleanup:
    orcm_finalize();
    return rc;
}

static void send_data(int fd, short flags, void *arg)
{
    int32_t count, *ptr;
    int rc;
    int j, n;
    opal_event_t *tmp = (opal_event_t*)arg;
    struct timeval now;
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
    opal_output(0, "%s sending msg number %d", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), msg_num);
    if (ORCM_SUCCESS != (rc = orcm_pnp.output(ORCM_PNP_GROUP_OUTPUT_CHANNEL, NULL,
                                              ORCM_PNP_TAG_OUTPUT, msg, count))) {
        ORTE_ERROR_LOG(rc);
    }

    msg_num++;
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


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
#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif

#include "opal/dss/dss.h"
#include "opal/mca/event/event.h"
#include "opal/util/output.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"
#include "orte/util/proc_info.h"
#include "orte/runtime/orte_globals.h"

#include "orte/mca/rml/rml.h"

#include "mca/pnp/pnp.h"
#include "mca/leader/leader.h"
#include "runtime/runtime.h"

#define ORCM_TEST_CLIENT_SERVER_TAG     110

/* local functions */
static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buf,
                       void *cbdata);

static void send_data(int fd, short flags, void *arg);

static void proc_failed(const char *stringid,
                        const orte_process_name_t *failed,
                        const orte_process_name_t *leader);

static int msg_num=0;
static struct timeval tp;
static int report_rate;

int main(int argc, char* argv[])
{
    int i;
    float pi;
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
    
    /* we want to listen to output from all versions and releases of the CLIENT app */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_receive("CLIENT", NULL, NULL,
                                                        ORCM_PNP_GROUP_OUTPUT_CHANNEL,
                                                        ORCM_PNP_TAG_OUTPUT, recv_input, NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* we will take input from anyone, and want notification whenever anyone fails */
    if (ORCM_SUCCESS != (rc = orcm_leader.set_policy("CLIENT", NULL, NULL,
                                                     ORTE_NAME_WILDCARD,
                                                     ORCM_NOTIFY_ANY, proc_failed))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* announce our existence */
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("SERVER", "1.0", "alpha", NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    opal_output(0, "SERVER %s ACTIVE - INCARNATION %d",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                orte_process_info.num_restarts);
    
    fprintf(stderr, "%s MY STDERR IS GOING HERE\n", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    printf("%s MY STDOUT IS GOING HERE\n", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));

    /* wake up every x seconds to send something */
    ORTE_TIMER_EVENT(tp.tv_sec, tp.tv_usec, send_data);

    /* just sit here */
    opal_event_dispatch(opal_event_base);
    
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

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buffer, void *cbdata)
{
    int32_t i, j, n, *data, *ptr;
    struct iovec *response;
    int rc;
    
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
        opal_output(0, "%s recvd data sender %s msg number %d",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    ORTE_NAME_PRINT(sender), data[0]);
    }

    /* see if we want to respond directly to the client */
    if (0 == (data[0] % 300)) {
        response = (struct iovec*)malloc(count * sizeof(struct iovec));
        for (j=0; j < count; j++) {
            response[j].iov_base = (void*)malloc(5 * sizeof(int32_t));
            ptr = response[j].iov_base;
            for (n=0; n < 5; n++) {
                *ptr = data[0];
                ptr++;
            }
            response[j].iov_len = 5 * sizeof(int32_t);
        }
        
        opal_output(0, "%s sending response %d direct to %s",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    data[0], ORTE_NAME_PRINT(sender));

        /* output the values */
        if (ORCM_SUCCESS != (rc = orcm_pnp.output(ORCM_PNP_GROUP_OUTPUT_CHANNEL, sender,
                                                  ORCM_TEST_CLIENT_SERVER_TAG,
                                                  response, count, NULL))) {
            ORTE_ERROR_LOG(rc);
        }
        for (j=0; j < count; j++) {
            free(response[j].iov_base);
        }
        free(response);
    }
}

static void cbfunc_mcast(int status, orte_process_name_t *name,
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
    
    /* output the values */
    if (0 == (msg_num % report_rate)) {
        opal_output(0, "%s mcasting data for msg number %d",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), msg_num);
    }

    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_GROUP_OUTPUT_CHANNEL, NULL,
                                                 ORCM_PNP_TAG_OUTPUT, msg, count, NULL, cbfunc_mcast, NULL))) {
        ORTE_ERROR_LOG(rc);
    }
    
    /* increment the msg number */
    msg_num++;
    
    /* reset the timer */
    opal_event_evtimer_add(tmp, &tp);
    
}

static void proc_failed(const char *stringid,
                        const orte_process_name_t *failed,
                        const orte_process_name_t *leader)
{
    opal_output(0, "%s Notified of proc %s failure",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                ORTE_NAME_PRINT(failed));
}


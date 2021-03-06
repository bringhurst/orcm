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
#include "opal/mca/event/event.h"
#include "opal/util/output.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"

#include "mca/pnp/pnp.h"
#include "runtime/runtime.h"

#define ORCM_TEST_CLIENT_SERVER_TAG     110
#define ORCM_TEST_CLIENT_CLIENT_TAG     120

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
static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buffer,
                       void *cbdata);
static void found_channel(const char *app, const char *version, const char *release,
                          orcm_pnp_channel_t channel);
static void responses(orcm_info_t *vm);

static int32_t flag=0;
static int msg_num;
static orcm_pnp_channel_t peer = ORCM_PNP_INVALID_CHANNEL;
static char *string_id=NULL;
static orte_process_name_t target;
static opal_event_t sigterm_handler, sigint_handler;

int main(int argc, char* argv[])
{
    struct timespec tp;
    int rc, delay;

    target.jobid = ORTE_JOBID_INVALID;
    target.vpid = ORTE_VPID_INVALID;

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

    /* for this application, register to hear messages sent to our input  */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_receive("client", "2.0", "beta",
                                                        ORCM_PNP_GROUP_INPUT_CHANNEL,
                                                        ORCM_TEST_CLIENT_SERVER_TAG, recv_input, NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* open a channel to any client 1.0 peers */
    if (ORCM_SUCCESS != (rc = orcm_pnp.open_channel("client", "1.0", NULL, ORTE_JOBID_WILDCARD, found_channel))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* announce our existence */
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("CLIENT", "2.0", "beta", responses))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    opal_output(0, "CLIENT2 %s ACTIVE", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));

    /* init the msg number */
    msg_num = 0;
    
    /* wake up every x seconds send something */
    ORTE_TIMER_EVENT(ORTE_PROC_MY_NAME->vpid + 1, 0, send_data);
    opal_event_dispatch(opal_event_base);

cleanup:
    orcm_finalize();
    return rc;
}

static void responses(orcm_info_t *vm)
{
    if (ORTE_JOBID_INVALID != target.jobid) {
        return;
    }
    if (0 == strcasecmp(vm->app, "client") &&
        0 == strcasecmp(vm->version, "1.0")) {
        opal_output(0, "%s ASSIGNING %s TO TARGET",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), ORTE_NAME_PRINT(vm->name));
        target.jobid = vm->name->jobid;
        target.vpid = vm->name->vpid;
        ORCM_CREATE_STRING_ID(&string_id, vm->app, vm->version, vm->release);
    }
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
    struct timeval now;

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
    if (ORTE_JOBID_INVALID == target.jobid) {
        opal_output(0, "%s mcasting data for msg number %d on GROUP output", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), msg_num);
        if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_GROUP_OUTPUT_CHANNEL, NULL,
                                                     ORCM_PNP_TAG_OUTPUT, msg, count,
                                                     NULL, cbfunc, NULL))) {
            ORTE_ERROR_LOG(rc);
        }
    } else {
        opal_output(0, "%s unicasting data for msg number %d on channel %d to %s of triplet %s",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), msg_num, peer, ORTE_NAME_PRINT(&target), string_id);
        if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(peer, &target,
                                                     ORCM_TEST_CLIENT_CLIENT_TAG, msg, count,
                                                     NULL, cbfunc, NULL))) {
            ORTE_ERROR_LOG(rc);
        }
    }
    
    /* increment the msg number */
    msg_num++;
    
    /* reset the timer */
    now.tv_sec = ORTE_PROC_MY_NAME->vpid + 1;
    now.tv_usec = 0;
    opal_event_evtimer_add(tmp, &now);
    
}

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buffer,
                       void *cbdata)
{
    opal_output(0, "%s recvd message from %s on tag %d",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                ORTE_NAME_PRINT(sender), (int)tag);
}

static void found_channel(const char *app, const char *version, const char *release,
                          orcm_pnp_channel_t channel)
{
    opal_output(0, "%s recvd channel %d for triplet %s:%s:%s",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                channel, app, version, release);
    ORCM_CREATE_STRING_ID(&string_id, app, version, release);
    peer = channel;
}

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
#include "opal/util/cmd_line.h"
#include "opal/util/opal_environ.h"

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

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buf,
                       void *cbdata);

static void found_channel(const char *app, const char *version,
                          const char *release,
                          orcm_pnp_channel_t channel);

static void send_data(int fd, short flags, void *arg);

static void responses(orcm_info_t *vm);

static int msg_num=0;
static int burst_num=0;
static int burst_size=250;
static int msg_size=100;
static int report_rate=100;
static int num_msgs_recvd=0;
static bool help;
static struct timeval starttime, stoptime;
static orcm_pnp_channel_t target=ORCM_PNP_INVALID_CHANNEL;
static opal_event_t sigterm_handler, sigint_handler;

static opal_cmd_line_init_t cmd_line_init[] = {
    /* Various "obvious" options */
    { NULL, NULL, NULL, 'h', NULL, "help", 0,
      &help, OPAL_CMD_LINE_TYPE_BOOL,
      "This help message" },
    { NULL, NULL, NULL, 'b', "burst-size", "burst-size", 1,
      &burst_size, OPAL_CMD_LINE_TYPE_INT,
      "Number of messages in a burst [default: 250]" },
    { NULL, NULL, NULL, 's', "message-size", "message-size", 1,
      &msg_size, OPAL_CMD_LINE_TYPE_INT,
      "Number of bytes in each message [default: 100]" },
    { NULL, NULL, NULL, 'r', "report-rate", "report-rate", 1,
      &report_rate, OPAL_CMD_LINE_TYPE_INT,
      "Number of bursts between printing a progress report [default: 100]" },
    /* End of list */
    { NULL, NULL, NULL, '\0', NULL, NULL, 0,
      NULL, OPAL_CMD_LINE_TYPE_NULL, NULL }
};

int main(int argc, char* argv[])
{
    opal_cmd_line_t cmd_line;
    int rc;
    char *args;

    opal_cmd_line_create(&cmd_line, cmd_line_init);
    mca_base_cmd_line_setup(&cmd_line);
    if (ORTE_SUCCESS != (rc = opal_cmd_line_parse(&cmd_line, true,
                                                  argc, argv)) ) {
        return rc;
    }

    if (help) {
        args = opal_cmd_line_get_usage_msg(&cmd_line);
        fprintf(stderr, "Usage: unicast_stress [OPTIONS]\n%s\n", args);
        free(args);
        exit(0);
    }

    /*
     * Since this process can now handle MCA/GMCA parameters, make sure to
     * process them.
     */
    mca_base_cmd_line_process_args(&cmd_line, &environ, &environ);
    
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

    /* register to recv anything sent to our input  */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_receive("UNICAST-STRESS", "1.0", "alpha",
                                                        ORCM_PNP_GROUP_INPUT_CHANNEL,
                                                        ORCM_PNP_TAG_WILDCARD, recv_input, NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* open a channel to send to a peer */
    if (ORCM_SUCCESS != (rc = orcm_pnp.open_channel("UNICAST-STRESS", "1.0", "alpha", ORTE_JOBID_WILDCARD, found_channel))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* announce our existence and get responses so we can know our peer is alive */
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("UNICAST-STRESS", "1.0", "alpha", responses))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    opal_output(0, "UNICAST-STRESS %s ACTIVE", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    
    /* just sit here */
    opal_event_dispatch(opal_event_base);
    
 cleanup:

    orcm_finalize();
    return rc;
}

static void found_channel(const char *app, const char *version,
                          const char *release,
                          orcm_pnp_channel_t channel)
{
    opal_output(0, "%s FOUND CHANNEL %d for app %s:%s:%s",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)channel, app, version, release);
    target = channel;
}

static void responses(orcm_info_t *vm)
{
    if (vm->name->jobid != ORTE_PROC_MY_NAME->jobid) {
        return;
    }

    /* need to get out of this callback so the system can prep
     * itself for messaging. If we are rank=0, then we use a
     * timer event for the very first message burst. This provides
     * a little delay to ensure both parties are ready. Only the
     * first burst is sent via timer - after that, it is driven
     * by receipt of an "ack" from the prior burst.
     */
    if (0 == ORTE_PROC_MY_NAME->vpid) {
        /* I will be sending the messages */
        ORTE_TIMER_EVENT(1, 0, send_data);
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

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       opal_buffer_t *buffer, void *cbdata)
{
    struct iovec *response;
    int rc;
    
    if (0 == ORTE_PROC_MY_NAME->vpid) {
        /* this is an ack - ready to send next burst */
        send_data(0, 0, NULL);
    } else {
        /* bump the number of recvd messages */
        num_msgs_recvd++;
        /* check if burst is complete */
        if (burst_size == num_msgs_recvd) {
            if (0 != burst_num && 0 == (burst_num % report_rate)) {
                opal_output(0, "%s acking burst number %d",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), burst_num);
            }
            burst_num++;
            /* send the ack */
            response = (struct iovec*)malloc(sizeof(struct iovec));
            response->iov_base = (void*)malloc(msg_size);
            response->iov_len = msg_size;
            if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_GROUP_INPUT_CHANNEL, sender,
                                                         ORCM_TEST_CLIENT_SERVER_TAG,
                                                         response, 1, NULL, cbfunc_mcast, NULL))) {
                ORTE_ERROR_LOG(rc);
            }
            num_msgs_recvd = 0;
        }
    }
}


static void send_data(int fd, short flags, void *arg)
{
    int rc;
    int n, i;
    struct iovec *msg;
    orte_process_name_t peer;
    long secs, usecs;
    float rate;

    /* only send if we got the channel */
    if (ORCM_PNP_INVALID_CHANNEL == target) {
        opal_output(0, "%s NO CHANNEL TO TARGET YET",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        ORTE_TIMER_EVENT(1, 0, send_data);
        return;
    }

    if (0 == burst_num) {
        gettimeofday(&starttime, NULL);
    } else if (0 == (burst_num % report_rate)) {
        gettimeofday(&stoptime, NULL);
        ORTE_COMPUTE_TIME_DIFF(secs, usecs, starttime.tv_sec, starttime.tv_usec, stoptime.tv_sec, stoptime.tv_usec);
        rate = (float)(burst_size * report_rate) / (float)(secs + (float)usecs/1000000.0);
        opal_output(0, "%s unicasting data for burst number %d: avg rate %5.2f msgs/sec",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), burst_num, rate);
        gettimeofday(&starttime, NULL);
    } 

    /* unicast a burst to my peer */
    for (i=0; i < burst_size; i++) {
        msg = (struct iovec*)malloc(sizeof(struct iovec));
        msg->iov_base = (void*)malloc(msg_size);
        msg->iov_len = msg_size;
    
        /* output the values */
        peer.jobid = ORTE_PROC_MY_NAME->jobid;
        peer.vpid = 1;

        if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(target, &peer,
                                                     ORCM_TEST_CLIENT_SERVER_TAG, msg, 1, NULL, cbfunc_mcast, NULL))) {
            ORTE_ERROR_LOG(rc);
        }
        msg_num++;
    }

    /* increment the burst number */
    burst_num++;
    
}

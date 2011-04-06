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
#include "orte/threads/threads.h"

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

static void* send_data(opal_object_t *obj);

static void responses(orcm_info_t *vm);

static int msg_num=0;
static int burst_num=0;
static int burst_size=250;
static int msg_size=100;
static int report_rate=1;
static int num_msgs_recvd=0;
static bool help;
static struct timeval starttime, stoptime;
static int send_pipe[2];
static orte_thread_ctl_t ctl;
static opal_thread_t send_thread;

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
      "Number of bursts between printing a progress report [default: 1]" },
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
        fprintf(stderr, "Usage: unicast_stress_threaded [OPTIONS]\n%s\n", args);
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
    
    /* start the send thread */
    OBJ_CONSTRUCT(&ctl, orte_thread_ctl_t);
    OBJ_CONSTRUCT(&send_thread, opal_thread_t);

    if (0 == ORTE_PROC_MY_NAME->vpid) {
        if (pipe(send_pipe) < 0) {
            opal_output(0, "%s Cannot open send thread pipe",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            rc = ORTE_ERROR;
            goto cleanup;
        }
        /* start the thread - we will send it a NULL msg pointer when
         * we want it to stop
         */
        send_thread.t_run = send_data;
        if (ORTE_SUCCESS != (rc = opal_thread_start(&send_thread))) {
            ORTE_ERROR_LOG(rc);
            ctl.running = false;
            rc = ORTE_ERROR;
            goto cleanup;
        }
    }

    /* register to recv anything sent to our input  */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_receive("UNICAST-STRESS", "1.0", "alpha",
                                                        ORCM_PNP_GROUP_INPUT_CHANNEL,
                                                        ORCM_PNP_TAG_WILDCARD, recv_input, NULL))) {
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

static void responses(orcm_info_t *vm)
{
    uint8_t flag=1;

    if (vm->name->jobid != ORTE_PROC_MY_NAME->jobid) {
        return;
    }

    if (0 == ORTE_PROC_MY_NAME->vpid) {
        /* I will be sending the messages */
        opal_fd_write(send_pipe[1], sizeof(uint8_t), &flag);
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
    uint8_t flag=1;

    if (0 == ORTE_PROC_MY_NAME->vpid) {
        /* this is an ack - ready to send next burst */
        opal_fd_write(send_pipe[1], sizeof(uint8_t), &flag);
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


static void* send_data(opal_object_t *obj)
{
    int rc;
    struct timespec tp={0, 10};
    int n, i;
    struct iovec *msg;
    orte_process_name_t peer;
    long secs, usecs;
    float rate;
    uint8_t flag;

    opal_output(0, "%s send thread operational", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));

    ORTE_ACQUIRE_THREAD(&ctl);
    ctl.running = true;
    ORTE_RELEASE_THREAD(&ctl);

    while (1) {
        /* block here until a trigger arrives */
        if (0 > (rc = opal_fd_read(send_pipe[0],
                                   sizeof(uint8_t), &flag))) {
            /* if something bad happened, punt */
            opal_output(0, "%s PUNTING THREAD", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            ORTE_ACQUIRE_THREAD(&ctl);
            ctl.running = false;
            ORTE_RELEASE_THREAD(&ctl);
            /* give a little delay to ensure the main thread gets into
             * opal_thread_join before we exit
             */
            nanosleep(&tp, NULL);
            return OPAL_THREAD_CANCELLED;
        }
        /* check to see if we were told to stop */
        if (0 == flag) {
            ORTE_ACQUIRE_THREAD(&ctl);
            ctl.running = false;
            ORTE_RELEASE_THREAD(&ctl);
            /* give a little delay to ensure the main thread gets into
             * opal_thread_join before we exit
             */
            nanosleep(&tp, NULL);
            return OPAL_THREAD_CANCELLED;
        }

        /* send the next burst */
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

            if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_GROUP_OUTPUT_CHANNEL, &peer,
                                                         ORCM_TEST_CLIENT_SERVER_TAG, msg, 1, NULL, cbfunc_mcast, NULL))) {
                ORTE_ERROR_LOG(rc);
            }
            msg_num++;
        }

        /* increment the burst number */
        burst_num++;
    }
    
}

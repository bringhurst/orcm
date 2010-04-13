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
#include "mca/leader/leader.h"
#include "runtime/runtime.h"

#define ORCM_TEST_CLIENT_SERVER_TAG     15

/* our message recv function */
static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg,
                       int count,
                       void *cbdata);

static void ldr_failed(char *app,
                       char *version,
                       char *release,
                       int sibling);

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
    
    /* announce our existence */
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("SERVER", "1.0", "alpha", NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* we want to accept ALL input messages from all versions and releases of client app */
    if (ORCM_SUCCESS != (rc = orcm_leader.set_leader("CLIENT", NULL, NULL,
                                                     ORCM_LEADER_WILDCARD, ldr_failed))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* we want to listen to all versions and releases of the CLIENT app */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_input("CLIENT", NULL, NULL,
                                                      ORCM_PNP_GROUP_OUTPUT_CHANNEL,
                                                      ORCM_PNP_TAG_OUTPUT, recv_input))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* just sit here */
    while (1) {
        for (i=0; i < 100000; i++) {
            pi = 3.14159 * (double)i;
        }
    }
    
cleanup:

    orcm_finalize();
    return rc;
}

static void cbfunc(int status, orte_process_name_t *name, orcm_pnp_tag_t tag,
                   struct iovec *msg, int count, void *cbdata)
{
    int i;
    
    opal_output(0, "%s mesg sent", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
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
                       struct iovec *msg,
                       int count,
                       void *cbdata)
{
    int32_t i, j, n, *data, *ptr;
    struct iovec *response;
    int rc;
    
    opal_output(0, "%s recvd message from client %s on tag %d with %d iovecs",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                ORTE_VPID_PRINT(sender->vpid), (int)tag, count);
    
    /* loop over the iovecs */
    for (i=0; i < count; i++) {
        /* check the number of values */
        if (20 != msg[i].iov_len) {
            opal_output(0, "\tError: iovec has incorrect length %d", (int)msg[i].iov_len);
            return;
        }
        
        /* print the first value */
        data = (int32_t*)msg[i].iov_base;
        opal_output(0, "\tValue in first posn: %d", data[0]);
        
        /* now check the values */
        for (n=1; n < 5; n++) {
            if (data[n] != data[0]) {
                opal_output(0, "\tError: invalid data %d at posn %d", data[n], n);
                return;
            }
        }
   }
    
    /* see if we want to respond directly to the client */
    if (1 == (data[0] % 5)) {
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
        
        /* output the values */
        opal_output(0, "%s sending response to %s for msg number %d",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    ORTE_NAME_PRINT(sender), data[0]);
        if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(ORCM_PNP_GROUP_OUTPUT_CHANNEL, sender,
                                                     ORCM_TEST_CLIENT_SERVER_TAG, msg, count, cbfunc, NULL))) {
            ORTE_ERROR_LOG(rc);
        }
    }
}

static void ldr_failed(char *app,
                       char *version,
                       char *release,
                       int sibling)
{
    opal_output(0, "%s LEADER FAILED", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
}

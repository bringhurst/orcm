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

/* our message recv function */
static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
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
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("LISTENER_IOVEC", "1.0", "alpha", NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* we want to listen to the TALKER app */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_input("TALKER_IOVEC", "1.0", "alpha",
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

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       struct iovec *msg, int count,
                       void *cbdata)
{
    int rc;
    
    opal_output(0, "%s recvd message from talker %s on tag %d",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                ORTE_NAME_PRINT(sender), (int)tag);
}

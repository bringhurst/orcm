/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config.h"
#include "include/constants.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rmcast/rmcast_types.h"

#include "mca/pnp/base/public.h"
#include "mca/pnp/base/private.h"

void orcm_pnp_base_push_data(orcm_pnp_source_t *src, opal_buffer_t *buf)
{
    src->msgs[src->end] = buf;
    /* move to next location, circling around if reqd */
    src->end = (1 + src->end) % ORCM_PNP_MAX_MSGS;
    return;
}

opal_buffer_t* orcm_pnp_base_pop_data(orcm_pnp_source_t *src)
{
    opal_buffer_t *buf;
    
    if (src->start == src->end) {
        /* no data available */
        return NULL;
    }
    
    /* save the location */
    buf = src->msgs[src->start];
    
    /* move to next location, circling around if reqd */
    src->start = (1 + src->start) % ORCM_PNP_MAX_MSGS;
}

bool orcm_pnp_base_valid_sequence_number(orcm_pnp_source_t *src,
                                         orte_rmcast_seq_t seq)
{
    if (ORTE_RMCAST_SEQ_INVALID == src->last_msg_num) {
        /* first message received */
        return true;
    }
    
    /* are we at the end of the sequence range? */
    if (ORTE_RMCAST_SEQ_MAX == src->last_msg_num) {
        /* then the next seq number better be 0 */
        if (0 != seq) {
            return false;
        }
    }
    
    /* check to see if the new one is 1 more than the old */
    if (seq != (src->last_msg_num + 1)) {
        return false;
    }
    
    /* yep - it is okay! */
    return true;
}

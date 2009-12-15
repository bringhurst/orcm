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

#include "orte/mca/rmcast/rmcast_types.h"

#include "mca/leader/base/private.h"


bool orcm_leader_base_valid_sequence_number(orcm_pnp_source_t *src,
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

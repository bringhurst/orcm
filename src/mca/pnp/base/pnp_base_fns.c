/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config_private.h"
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

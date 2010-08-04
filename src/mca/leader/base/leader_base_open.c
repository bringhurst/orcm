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
#include "orte/types.h"

#include "opal/class/opal_value_array.h"
#include "opal/util/output.h"
#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"

#include "mca/leader/leader.h"
#include "mca/leader/base/public.h"
#include "mca/leader/base/private.h"

/* instantiate the module */
orcm_leader_base_module_t orcm_leader = {
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

/* instantiate the globals */
orcm_leader_base_t orcm_leader_base;

int orcm_leader_base_open(void)
{
    /* Debugging / verbose output.  Always have stream open, with
     verbose set by the mca open system... */
    orcm_leader_base.output = opal_output_open(NULL);
    
    /* Open up all available components */
    if (ORCM_SUCCESS != 
        mca_base_components_open("orcm_leader", orcm_leader_base.output, NULL,
                                 &orcm_leader_base.opened, true)) {
            return ORCM_ERROR;
        }
    
    /* All done */
    return ORCM_SUCCESS;
}

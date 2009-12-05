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

#include "opal/class/opal_value_array.h"
#include "opal/util/output.h"
#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"

#include "mca/leader/leader.h"
#include "mca/leader/base/public.h"
#include "mca/leader/base/private.h"
#include "mca/leader/base/components.h"

const mca_base_component_t *orcm_leader_base_components[] = {
    &mca_leader_fifl_component.super.leaderc_version,
    NULL
};

/* instantiate the module */
orcm_leader_base_module_t orcm_leader = {
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
        mca_base_components_open("leader", orcm_leader_base.output,
                                 orcm_leader_base_components, 
                                 &orcm_leader_base.opened, true)) {
            return ORCM_ERROR;
        }
    
    /* All done */
    return ORCM_SUCCESS;
}

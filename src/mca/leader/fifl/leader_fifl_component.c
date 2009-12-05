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

#include "opal/util/output.h"

#include "mca/leader/leader.h"
#include "mca/leader/fifl/leader_fifl.h"

orcm_leader_fifl_component_t mca_leader_fifl_component = {
    {
        /* First, the mca_base_component_t struct containing meta
            information about the component itself */
        
        {
            ORCM_LEADER_BASE_VERSION_2_0_0,
            
            "fifl",
            OPENRCM_MAJOR_VERSION,
            OPENRCM_MINOR_VERSION,
            OPENRCM_RELEASE_VERSION,
            orcm_leader_fifl_component_open,
            orcm_leader_fifl_component_close,
            orcm_leader_fifl_component_query,
            orcm_leader_fifl_component_register
        },
        {
            /* The component is checkpoint ready */
            MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};

int orcm_leader_fifl_component_open(void)
{
    mca_base_component_t *c = &mca_leader_fifl_component.super.leaderc_version;
    
    /* lookup parameters */
    mca_base_param_reg_int(c, "trigger",
                           "Difference level between recvd packet numbers that declares a leader as unavailable",
                           false, false, NULL,  &mca_leader_fifl_component.trigger);
    
    return ORCM_SUCCESS;
}

int orcm_leader_fifl_component_close(void)
{
    return ORCM_SUCCESS;
}

int orcm_leader_fifl_component_query(mca_base_module_t **module, int *priority)
{
    *module = (mca_base_module_t*)&orcm_leader_fifl_module;
    *priority = 50;
    return ORCM_SUCCESS;
}

int orcm_leader_fifl_component_register(void)
{
    return ORCM_SUCCESS;
}


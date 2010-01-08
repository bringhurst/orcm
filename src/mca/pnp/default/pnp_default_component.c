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

#include "opal/util/output.h"

#include "mca/pnp/pnp.h"
#include "mca/pnp/default/pnp_default.h"

orcm_pnp_base_component_t mca_pnp_default_component = {
    /* First, the mca_base_component_t struct containing meta
       information about the component itself */
    
    {
        ORCM_PNP_BASE_VERSION_2_0_0,
        
        "default",
        OPENRCM_MAJOR_VERSION,
        OPENRCM_MINOR_VERSION,
        OPENRCM_RELEASE_VERSION,
        orcm_pnp_default_component_open,
        orcm_pnp_default_component_close,
        orcm_pnp_default_component_query,
        orcm_pnp_default_component_register
    },
    {
        /* The component is checkpoint ready */
        MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

int orcm_pnp_default_component_open(void)
{
    return ORCM_SUCCESS;
}

int orcm_pnp_default_component_close(void)
{
    return ORCM_SUCCESS;
}

int orcm_pnp_default_component_query(mca_base_module_t **module, int *priority)
{
    *module = (mca_base_module_t*)&orcm_pnp_default_module;
    *priority = 50;
    return ORCM_SUCCESS;
}

int orcm_pnp_default_component_register(void)
{
    return ORCM_SUCCESS;
}


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

#include "mca/clip/clip.h"
#include "mca/clip/default/clip_default.h"

orcm_clip_base_component_t mca_clip_default_component = {
    /* First, the mca_base_component_t struct containing meta
       information about the component itself */
    
    {
        ORCM_CLIP_BASE_VERSION_2_0_0,
        
        "default",
        OPENRCM_MAJOR_VERSION,
        OPENRCM_MINOR_VERSION,
        OPENRCM_RELEASE_VERSION,
        orcm_clip_default_component_open,
        orcm_clip_default_component_close,
        orcm_clip_default_component_query,
        orcm_clip_default_component_register
    },
    {
        /* The component is checkpoint ready */
        MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

int orcm_clip_default_component_open(void)
{
    return ORCM_SUCCESS;
}

int orcm_clip_default_component_close(void)
{
    return ORCM_SUCCESS;
}

int orcm_clip_default_component_query(mca_base_module_t **module, int *priority)
{
    *module = (mca_base_module_t*)&orcm_clip_default_module;
    *priority = 10;
    return ORCM_SUCCESS;
}

int orcm_clip_default_component_register(void)
{
    return ORCM_SUCCESS;
}


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

#include "runtime/runtime.h"

#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/tool/cfgi_tool.h"

orcm_cfgi_tool_component_t mca_orcm_cfgi_tool_component = {
    {
        /* First, the mca_base_component_t struct containing meta
         information about the component itself */

        {
            ORCM_CFGI_BASE_VERSION_2_0_0,

            "tool",
            OPENRCM_MAJOR_VERSION,
            OPENRCM_MINOR_VERSION,
            OPENRCM_RELEASE_VERSION,
            orcm_cfgi_tool_component_open,
            orcm_cfgi_tool_component_close,
            orcm_cfgi_tool_component_query,
            orcm_cfgi_tool_component_register
        },
        {
            /* The component is checkpoint ready */
            MCA_BASE_METADATA_PARAM_CHECKPOINT
        }
    }
};

int orcm_cfgi_tool_component_open(void)
{
    return ORCM_SUCCESS;
}

int orcm_cfgi_tool_component_close(void)
{
    return ORCM_SUCCESS;
}

int orcm_cfgi_tool_component_query(mca_base_module_t **module, int *priority)
{
    if (ORCM_PROC_IS_DAEMON) {
        *module = (mca_base_module_t*)&orcm_cfgi_tool_module;
        *priority = 10;
        return ORCM_SUCCESS;
    }

    /* otherwise, cannot use this module */
    *priority = 0;
    *module = NULL;
    return ORCM_ERROR;
}

int orcm_cfgi_tool_component_register(void)
{
    return ORCM_SUCCESS;
}


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

#include "mca/leader/leader.h"
#include "mca/leader/lowest/leader_lowest.h"

static int component_open(void);
static int component_close(void);
static int component_query(mca_base_module_2_0_0_t **module, int *priority);
static int component_register(void);


orcm_leader_base_component_t mca_orcm_leader_lowest_component = {
    {
        ORCM_LEADER_BASE_VERSION_2_0_0,
            
        "lowest",
        OPENRCM_MAJOR_VERSION,
        OPENRCM_MINOR_VERSION,
        OPENRCM_RELEASE_VERSION,
        component_open,
        component_close,
        component_query,
        component_register
    },
    {
        /* The component is checkpoint ready */
        MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int component_open(void)
{
    return ORCM_SUCCESS;
}

static int component_close(void)
{
    return ORCM_SUCCESS;
}

static int component_query(mca_base_module_t **module, int *priority)
{
    *module = (mca_base_module_t*)&orcm_leader_lowest_module;
    *priority = 10;

    return ORCM_SUCCESS;
}

static int component_register(void)
{
    return ORCM_SUCCESS;
}


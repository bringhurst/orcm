/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
 *
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "openrcm_config_private.h"
#include "include/constants.h"

#include "opal/mca/base/mca_base_param.h"

#include "orte/util/proc_info.h"
#include "orte/runtime/orte_globals.h"

#include "orte/mca/ess/ess.h"
#include "orte/mca/ess/orcm/ess_orcm.h"

#include "runtime/runtime.h"

extern orte_ess_base_module_t orte_ess_orcm_module;

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */
orte_ess_base_component_t mca_ess_orcm_component = {
    {
        ORTE_ESS_BASE_VERSION_2_0_0,
            
        /* Component name and version */
        "orcm",
        ORTE_MAJOR_VERSION,
        ORTE_MINOR_VERSION,
        ORTE_RELEASE_VERSION,
            
        /* Component open and close functions */
        orte_ess_orcm_component_open,
        orte_ess_orcm_component_close,
        orte_ess_orcm_component_query
    },
    {
        /* The component is checkpoint ready */
        MCA_BASE_METADATA_PARAM_CHECKPOINT
    }
};


int
orte_ess_orcm_component_open(void)
{
    return ORTE_SUCCESS;
}


int orte_ess_orcm_component_query(mca_base_module_t **module, int *priority)
{
    /* if we are an ORCM master, then we are available */
    if (ORCM_PROC_IS_MASTER) {
        *priority = 1000;
        *module = (mca_base_module_t *)&orte_ess_orcm_module;
        return ORTE_SUCCESS;
    }
    *priority = 0;
    *module = NULL;
    return ORTE_ERROR;
}


int
orte_ess_orcm_component_close(void)
{
    return ORTE_SUCCESS;
}


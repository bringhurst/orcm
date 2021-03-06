/*
 * Copyright (c) 2010      Cisco Systems, Inc. All rights reserved.
 *
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */
#include "openrcm.h"

#include "orte_config.h"
#include "opal/util/output.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/errmgr/base/base.h"

#include "runtime/runtime.h"
#include "errmgr_sched.h"

/*
 * Public string for version number
 */
const char *orte_errmgr_orcmsched_component_version_string = 
    "ORTE ERRMGR orcmsched MCA component version " ORTE_VERSION;

/*
 * Local functionality
 */
static int errmgr_sched_open(void);
static int errmgr_sched_close(void);
static int errmgr_sched_component_query(mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
orte_errmgr_base_component_t mca_errmgr_orcmsched_component =
{
    /* Handle the general mca_component_t struct containing 
     *  meta information about the component
     */
    {
        ORTE_ERRMGR_BASE_VERSION_3_0_0,
        /* Component name and version */
        "orcmsched",
        ORTE_MAJOR_VERSION,
        ORTE_MINOR_VERSION,
        ORTE_RELEASE_VERSION,
        
        /* Component open and close functions */
        errmgr_sched_open,
        errmgr_sched_close,
        errmgr_sched_component_query
    },
    {
        /* The component is checkpoint ready */
        MCA_BASE_METADATA_PARAM_CHECKPOINT
    },

    /* Verbosity level */
    0,
    /* opal_output handler */
    -1,
    /* Default priority */
    1
};

static int errmgr_sched_open(void) 
{
    return ORTE_SUCCESS;
}

static int errmgr_sched_close(void)
{
    return ORTE_SUCCESS;
}

static int errmgr_sched_component_query(mca_base_module_t **module, int *priority)
{
    /* if we are a scheduler, this is the default module */
    if (ORCM_PROC_IS_SCHEDULER) {
        *priority = 1000;
        *module = (mca_base_module_t *)&orte_errmgr_orcmsched_module;
        return ORTE_SUCCESS;
    }

    *priority = 0;
    *module = NULL;
    return ORTE_ERROR;
    
}


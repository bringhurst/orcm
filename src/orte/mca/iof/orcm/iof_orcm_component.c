/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2010 Sun Microsystems, Inc.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config_private.h"
#include "include/constants.h"

#include "orte_config.h"

#include "opal/mca/base/base.h"
#include "opal/util/output.h"
#include "opal/mca/base/mca_base_param.h"

#include "orte/util/proc_info.h"
#include "orte/runtime/orte_globals.h"

#include "orte/mca/iof/base/base.h"

#include "runtime/runtime.h"

#include "iof_orcm.h"

/*
 * Local functions
 */
static int orte_iof_orcm_open(void);
static int orte_iof_orcm_close(void);
static int orte_iof_orcm_query(mca_base_module_t **module, int *priority);

/*
 * Public string showing the iof orcm component version number
 */
const char *mca_iof_orcm_component_version_string =
    "Open MPI orcm iof MCA component version " ORTE_VERSION;

orte_iof_orcm_component_t mca_iof_orcm_component = {
    {
        /* First, the mca_base_component_t struct containing meta
         information about the component itself */
        
        {
            ORTE_IOF_BASE_VERSION_2_0_0,
            
            "orcm", /* MCA component name */
            ORTE_MAJOR_VERSION,  /* MCA component major version */
            ORTE_MINOR_VERSION,  /* MCA component minor version */
            ORTE_RELEASE_VERSION,  /* MCA component release version */
            
            /* Component open, close, and query functions */
            orte_iof_orcm_open,
            orte_iof_orcm_close,
            orte_iof_orcm_query 
        },
        {
            /* The component is checkpoint ready */
            MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
        
    }
};

/**
  * component open/close/init function
  */
static int orte_iof_orcm_open(void)
{
    /* Nothing to do */
    return ORTE_SUCCESS;
}


static int orte_iof_orcm_close(void)
{
    return ORTE_SUCCESS;
}

/**
 * Module query
 */

static int orte_iof_orcm_query(mca_base_module_t **module, int *priority)
{
    /* if we are an IOF endpt, then use this module */
    if (ORCM_PROC_IS_IOF_ENDPT) {
        *priority = 100;
        *module = (mca_base_module_t *) &orte_iof_orcm_module;
        return ORTE_SUCCESS;
    }
        
    *priority = 0;
    *module = NULL;
    return ORTE_ERROR;
}

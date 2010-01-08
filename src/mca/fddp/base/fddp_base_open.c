/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "openrcm_config_private.h"
#include "constants.h"

#include "opal/mca/mca.h"
#include "opal/util/output.h"
#include "opal/mca/base/base.h"
#include "opal/mca/base/mca_base_param.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "mca/fddp/base/public.h"
#include "mca/fddp/base/components.h"


const mca_base_component_t *orcm_fddp_base_components[] = {
    &mca_fddp_trend_component.super.base_version,
    NULL
};


/*
 * Global variables
 */
int orcm_fddp_base_output = -1;
orcm_fddp_base_module_t orcm_fddp;
opal_list_t mca_fddp_base_components_available;
orcm_fddp_base_component_t mca_fddp_base_selected_component;


/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
int orcm_fddp_base_open(void)
{
    /* Debugging / verbose output.  Always have stream open, with
       verbose set by the mca open system... */
    orcm_fddp_base_output = opal_output_open(NULL);
    
    /* Open up all available components */

    if (ORTE_SUCCESS !=
        mca_base_components_open("fddp", orcm_fddp_base_output,
                                 orcm_fddp_base_components,
                                 &mca_fddp_base_components_available, true)) {
        return ORTE_ERROR;
    }

    /* All done */

    return ORTE_SUCCESS;
}

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

#include <stdio.h>

#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"

#include "mca/fddp/base/public.h"

int orcm_fddp_base_close(void)
{
    /* If we have a selected component and module, then finalize it */
    
    if (NULL != orcm_fddp.finalize) {
        orcm_fddp.finalize();
    }
    
    /* Close all remaining available components */
    
    mca_base_components_close(orcm_fddp_base_output, 
                              &mca_fddp_base_components_available, NULL);
    
    /* All done */
    
    return ORTE_SUCCESS;
}

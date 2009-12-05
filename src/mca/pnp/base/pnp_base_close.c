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

#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"

#include "mca/pnp/pnp.h"
#include "mca/pnp/base/public.h"
#include "mca/pnp/base/private.h"

int orcm_pnp_base_close(void)
{
    if (NULL != orcm_pnp.finalize) {
        orcm_pnp.finalize();
    }
    
    /* Close all remaining available components (may be one if this is a
     Open RTE program, or [possibly] multiple if this is ompi_info) */
    
    mca_base_components_close(orcm_pnp_base.output, 
                              &orcm_pnp_base.opened, NULL);
    
    return ORCM_SUCCESS;
}

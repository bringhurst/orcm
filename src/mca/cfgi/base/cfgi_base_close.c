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

#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/base/public.h"
#include "mca/cfgi/base/private.h"

int orcm_cfgi_base_close(void)
{
    if (NULL != orcm_cfgi.finalize) {
        orcm_cfgi.finalize();
    }
    
    /* Close all remaining available components (may be one if this is a
     Open RTE program, or [possibly] multiple if this is ompi_info) */
    
    mca_base_components_close(orcm_cfgi_base.output, 
                              &orcm_cfgi_base.opened, NULL);
    
    return ORCM_SUCCESS;
}

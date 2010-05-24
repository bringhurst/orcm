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

#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"

#include "mca/leader/leader.h"
#include "mca/leader/base/public.h"
#include "mca/leader/base/private.h"

int orcm_leader_base_close(void)
{
    if (NULL != orcm_leader.finalize) {
        orcm_leader.finalize();
    }
    
    /* Close all remaining available components (may be one if this is a
     Open RTE program, or [possibly] multiple if this is ompi_info) */
    
    mca_base_components_close(orcm_leader_base.output, 
                              &orcm_leader_base.opened, NULL);
    
    return ORCM_SUCCESS;
}

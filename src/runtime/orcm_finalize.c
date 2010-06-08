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

#include "opal/runtime/opal.h"

#include "orte/runtime/runtime.h"

#include "mca/pnp/base/public.h"

#include "runtime/runtime.h"

int orcm_finalize(void)
{
    if (orcm_util_initialized) {
        opal_finalize_util();
        orcm_util_initialized = false;
    }
    
    if (!orcm_initialized) {
        return ORCM_SUCCESS;
    }
    
    /* remove all signal handlers */
    orcm_remove_signal_handlers();

    if (ORTE_PROC_IS_APP) {
        orcm_pnp_base_close();
    }

    orte_finalize();
    
    orcm_initialized = false;
    return ORCM_SUCCESS;
}

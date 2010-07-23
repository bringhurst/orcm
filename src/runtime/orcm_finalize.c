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

#include "runtime/runtime.h"

int orcm_finalize(void)
{
    if (!orcm_initialized) {
        return ORCM_SUCCESS;
    }

    /* remove all signal handlers */
    orcm_remove_signal_handlers();

    orte_finalize();
    
    orcm_initialized = false;
    return ORCM_SUCCESS;
}

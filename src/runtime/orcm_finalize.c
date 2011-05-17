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

#include "mca/pnp/pnp.h"
#include "runtime/runtime.h"

int orcm_finalize(void)
{
    if (!orcm_initialized) {
        return ORCM_SUCCESS;
    }

    /* cleanup the environ */
    unsetenv("OMPI_MCA_rmcast_base_if_include");

    /* remove all signal handlers */
    orcm_remove_signal_handlers();

    /* stop messaging */
    orcm_pnp.disable_comm();

    orte_finalize();
    
    orcm_initialized = false;
    return ORCM_SUCCESS;
}

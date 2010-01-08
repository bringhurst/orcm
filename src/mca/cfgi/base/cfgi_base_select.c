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

#include "orte/mca/errmgr/errmgr.h"

#include "mca/cfgi/base/public.h"
#include "mca/cfgi/base/private.h"

static bool selected = false;

/*
 * Select one PNP component from all those that are available.
 */
int orcm_cfgi_base_select(void)
{
    mca_base_component_t *best_component = NULL;
    orcm_cfgi_base_module_t *best_module = NULL;
    int rc;

    if (selected) {
        /* ensure we don't do this twice */
        return ORCM_SUCCESS;
    }
    selected = true;
    
    /*
     * Select the best component
     */
    if( ORCM_SUCCESS != mca_base_select("cfgi", orcm_cfgi_base.output,
                                        &orcm_cfgi_base.opened,
                                        (mca_base_module_t **) &best_module,
                                        (mca_base_component_t **) &best_component) ) {
        /* This will only happen if no component was selected - that is okay here */
        return ORCM_SUCCESS;
    }

    orcm_cfgi = *best_module;
    
    /* init the selected module */
    if (NULL != orcm_cfgi.init) {
        if (ORCM_SUCCESS != (rc = orcm_cfgi.init())) {
            ORTE_ERROR_LOG(rc);
            return rc;
        }
    }

    return ORCM_SUCCESS;
}

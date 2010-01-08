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
#include "opal/mca/base/base.h"

#include "mca/fddp/base/public.h"


/**
 * Function for selecting one component from all those that are
 * available.
 */
int orcm_fddp_base_select(void)
{
    int ret, exit_status = ORTE_SUCCESS;
    orcm_fddp_base_component_t *best_component = NULL;
    orcm_fddp_base_module_t *best_module = NULL;
    char *include_list = NULL;

    /*
     * Register the framework MCA param and look up include list
     */
    mca_base_param_reg_string_name("fddp", NULL,
                                   "Which fddp component to use (empty = none)",
                                   false, false,
                                   NULL, &include_list);
    
    /* If we do not have any components to select this is ok. Just use the default
     * "no-op" component and move on.
     */
    if( 0 >= opal_list_get_size(&mca_fddp_base_components_available) || NULL == include_list) { 
        /* Close all components since none will be used */
        mca_base_components_close(0, /* Pass 0 to keep this from closing the output handle */
                                  &mca_fddp_base_components_available,
                                  NULL);
        goto cleanup;
    }
    
    /*
     * Select the best component
     */
    if( ORTE_SUCCESS != mca_base_select("fddp", orcm_fddp_base_output,
                                        &mca_fddp_base_components_available,
                                        (mca_base_module_t **) &best_module,
                                        (mca_base_component_t **) &best_component) ) {
        /* It is okay if no component was selected - we just leave
         * the orcm_fddp module as the default
         */
        exit_status = ORTE_SUCCESS;
        goto cleanup;
    }

    if (NULL != orcm_fddp.init) {
        /* if an init function is provided, use it */
        if (ORTE_SUCCESS != (ret = orcm_fddp.init()) ) {
            exit_status = ret;
            goto cleanup;
        }
    }

    /* Save the winner */
    orcm_fddp = *best_module;

 cleanup:
    return exit_status;
}

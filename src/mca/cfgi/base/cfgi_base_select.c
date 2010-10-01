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
    opal_list_item_t *item;
    mca_base_component_list_item_t *cli = NULL;
    mca_base_component_t *component = NULL;
    mca_base_module_t *module = NULL;
    orcm_cfgi_base_module_t *nmodule;
    orcm_cfgi_base_selected_module_t *newmodule;
    int rc, priority;

    if (selected) {
        /* ensure we don't do this twice */
        return ORCM_SUCCESS;
    }
    selected = true;
    
    /* Query all available components and ask if they have a module */
    for (item = opal_list_get_first(&orcm_cfgi_components_available);
         opal_list_get_end(&orcm_cfgi_components_available) != item;
         item = opal_list_get_next(item)) {
        cli = (mca_base_component_list_item_t *) item;
        component = (mca_base_component_t *) cli->cli_component;

        /* If there's no query function, skip it */
        if (NULL == component->mca_query_component) {
            opal_output_verbose(5, orcm_cfgi_base.output,
                                "mca:cfgi:select: Skipping component [%s]. It does not implement a query function",
                                component->mca_component_name );
            continue;
        }

        /* Query the component */
        opal_output_verbose(5, orcm_cfgi_base.output,
                            "mca:cfgi:select: Querying component [%s]",
                            component->mca_component_name);
        rc = component->mca_query_component(&module, &priority);

        /* If no module was returned, then skip component */
        if (ORCM_SUCCESS != rc || NULL == module) {
            opal_output_verbose(5, orcm_cfgi_base.output,
                                "mca:cfgi:select: Skipping component [%s]. Query failed to return a module",
                                component->mca_component_name );
            continue;
        }

        /* If we got a module, initialize it */
        nmodule = (orcm_cfgi_base_module_t*) module;
        if (NULL != nmodule->init) {
            if (ORCM_SUCCESS == nmodule->init()) {
                /* add to the list of selected modules */
                newmodule = OBJ_NEW(orcm_cfgi_base_selected_module_t);
                newmodule->module = nmodule;
                opal_list_append(&orcm_cfgi_selected_modules, &newmodule->super);
            } else {
                /* If the module doesn't want to be used, skip it */
                if (NULL != nmodule->finalize) {
                    nmodule->finalize();
                }
            }
        }
    }

    return ORCM_SUCCESS;
}

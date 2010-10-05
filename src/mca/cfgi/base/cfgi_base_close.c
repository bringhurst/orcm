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

#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/base/public.h"
#include "mca/cfgi/base/private.h"

int orcm_cfgi_base_close(void)
{
    opal_list_item_t *item;
    orcm_cfgi_base_selected_module_t *nmodule;

    for (item = opal_list_remove_first(&orcm_cfgi_selected_modules);
         NULL != item;
         item = opal_list_remove_first(&orcm_cfgi_selected_modules)) {
        nmodule = (orcm_cfgi_base_selected_module_t*) item;
        if (NULL != nmodule->module->finalize) {
            nmodule->module->finalize();
        }
        OBJ_RELEASE(nmodule);
    }
    OBJ_DESTRUCT(&orcm_cfgi_selected_modules);    

    /* Close all remaining available components */
    
    mca_base_components_close(orcm_cfgi_base.output, 
                              &orcm_cfgi_components_available, NULL);
    
    OBJ_DESTRUCT(&orcm_cfgi_base.lock);
    OBJ_DESTRUCT(&orcm_cfgi_base.cond);

    return ORCM_SUCCESS;
}

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
    int i;
    orcm_cfgi_app_t *app;
    orcm_cfgi_run_t *run;

    /* stop the launch event */
    if (-1 != orcm_cfgi_base.launch_pipe[0]) {
        opal_event_del(&orcm_cfgi_base.launch_event);
    }

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

    /* release any installed apps */
    for (i=0; i < orcm_cfgi_base.installed_apps.size; i++) {
        if (NULL == (app = (orcm_cfgi_app_t*)opal_pointer_array_get_item(&orcm_cfgi_base.installed_apps, i))) {
            continue;
        }
        OBJ_RELEASE(app);
    }
    OBJ_DESTRUCT(&orcm_cfgi_base.installed_apps);

    /* release any configd apps */
    for (i=0; i < orcm_cfgi_base.confgd_apps.size; i++) {
        if (NULL == (run = (orcm_cfgi_run_t*)opal_pointer_array_get_item(&orcm_cfgi_base.confgd_apps, i))) {
            continue;
        }
        OBJ_RELEASE(run);
    }
    OBJ_DESTRUCT(&orcm_cfgi_base.confgd_apps);

    /* Close all remaining available components */
    
    mca_base_components_close(orcm_cfgi_base.output, 
                              &orcm_cfgi_components_available, NULL);
    
    OBJ_DESTRUCT(&orcm_cfgi_base.ctl);

    return ORCM_SUCCESS;
}

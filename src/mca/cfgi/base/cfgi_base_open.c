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

#include "opal/util/output.h"
#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"

#include "runtime/runtime.h"

#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/base/public.h"
#include "mca/cfgi/base/private.h"
#include "mca/cfgi/base/components.h"

/* stubs */
static int orcm_stub_init(void);
static int orcm_stub_finalize(void);

/* instantiate the module */
orcm_cfgi_base_module_t orcm_cfgi = {
    orcm_stub_init,
    orcm_stub_finalize,
};

/* instantiate the globals */
orcm_cfgi_base_t orcm_cfgi_base;
opal_list_t orcm_cfgi_components_available;
opal_list_t orcm_cfgi_selected_modules;

int orcm_cfgi_base_open(void)
{
    /* Debugging / verbose output.  Always have stream open, with
     verbose set by the mca open system... */
    orcm_cfgi_base.output = opal_output_open(NULL);
    
    /* init the globals */
    OBJ_CONSTRUCT(&orcm_cfgi_base.lock, opal_mutex_t);
    OBJ_CONSTRUCT(&orcm_cfgi_base.cond, opal_condition_t);
    orcm_cfgi_base.active = false;
    orcm_cfgi_base.num_active_apps = 0;
    orcm_cfgi_base.daemons = NULL;
    OBJ_CONSTRUCT(&orcm_cfgi_components_available, opal_list_t);
    OBJ_CONSTRUCT(&orcm_cfgi_selected_modules, opal_list_t);

    /* Open up all available components */
    if (ORCM_SUCCESS != 
        mca_base_components_open("orcm_cfgi", orcm_cfgi_base.output, NULL,
                                 &orcm_cfgi_components_available, true)) {
            return ORCM_ERROR;
        }
    
    /* All done */
    return ORCM_SUCCESS;
}

static int orcm_stub_init(void)
{
    return ORCM_SUCCESS;
}

static int orcm_stub_finalize(void)
{
    return ORCM_SUCCESS;
}


OBJ_CLASS_INSTANCE(orcm_cfgi_base_selected_module_t,
                   opal_list_item_t,
                   NULL, NULL);

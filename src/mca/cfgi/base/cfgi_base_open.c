/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config.h"
#include "include/constants.h"

#include "opal/util/output.h"
#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"

#include "runtime/runtime.h"

#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/base/public.h"
#include "mca/cfgi/base/private.h"
#include "mca/cfgi/base/components.h"

const mca_base_component_t *orcm_cfgi_base_components[] = {
    &mca_cfgi_file_component.super.cfgic_version,
    NULL
};

/* stubs */
static int orcm_stub_init(void);
static void orcm_stub_read_config(orcm_spawn_fn_t spawn_apps);
static int orcm_stub_finalize(void);

/* instantiate the module */
orcm_cfgi_base_module_t orcm_cfgi = {
    orcm_stub_init,
    orcm_stub_read_config,
    orcm_stub_finalize,

};

/* instantiate the globals */
orcm_cfgi_base_t orcm_cfgi_base;

int orcm_cfgi_base_open(void)
{
    /* Debugging / verbose output.  Always have stream open, with
     verbose set by the mca open system... */
    orcm_cfgi_base.output = opal_output_open(NULL);
    
    /* Open up all available components */
    if (ORCM_SUCCESS != 
        mca_base_components_open("cfgi", orcm_cfgi_base.output,
                                 orcm_cfgi_base_components, 
                                 &orcm_cfgi_base.opened, true)) {
            return ORCM_ERROR;
        }
    
    /* All done */
    return ORCM_SUCCESS;
}

static int orcm_stub_init(void)
{
    return;
}

static void orcm_stub_read_config(orcm_spawn_fn_t spawn_apps)
{
    return;
}

static int orcm_stub_finalize(void)
{
    return;
}

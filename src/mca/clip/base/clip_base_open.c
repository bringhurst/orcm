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

#include "mca/clip/clip.h"
#include "mca/clip/base/public.h"
#include "mca/clip/base/private.h"
#include "mca/clip/base/components.h"

/* instantiate the module */
orcm_clip_base_module_t orcm_clip = {
    NULL,
    NULL,
    NULL,

};

/* instantiate the globals */
orcm_clip_base_t orcm_clip_base;

int orcm_clip_base_open(void)
{
    /* Debugging / verbose output.  Always have stream open, with
     verbose set by the mca open system... */
    orcm_clip_base.output = opal_output_open(NULL);
    
    /* Open up all available components */
    if (ORCM_SUCCESS != 
        mca_base_components_open("orcm_clip", orcm_clip_base.output, NULL,
                                 &orcm_clip_base.opened, true)) {
            return ORCM_ERROR;
        }
    
    /* All done */
    return ORCM_SUCCESS;
}

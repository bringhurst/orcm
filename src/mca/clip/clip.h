/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef ORCM_CLIP_H
#define ORCM_CLIP_H

#include "openrcm.h"

#include "opal/mca/mca.h"

/* module functions */
typedef int (*orcm_clip_module_init_fn_t)(void);
typedef int (*orcm_clip_module_finalize_fn_t)(void);

typedef int (*orcm_clip_module_replicate_fn_t)(void);

/* component struct */
typedef struct {
    /** Base component description */
    mca_base_component_t clipc_version;
    /** Base component data block */
    mca_base_component_data_t clipc_data;
} orcm_clip_base_component_2_0_0_t;
/** Convenience typedef */
typedef orcm_clip_base_component_2_0_0_t orcm_clip_base_component_t;

/* module struct */
typedef struct {
    orcm_clip_module_init_fn_t        init;
    orcm_clip_module_replicate_fn_t   replicate;
    orcm_clip_module_finalize_fn_t    finalize;
} orcm_clip_base_module_t;

/** Interface for LEADER selection */
ORCM_DECLSPEC extern orcm_clip_base_module_t orcm_clip;

/*
 * Macro for use in components that are of type coll
 */
#define ORCM_CLIP_BASE_VERSION_2_0_0 \
  MCA_BASE_VERSION_2_0_0, \
  "orcm_clip", 2, 0, 0

#endif /* ORCM_CLIP_H */

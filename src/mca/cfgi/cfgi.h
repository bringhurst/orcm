/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef ORCM_CFGI_H
#define ORCM_CFGI_H

#include "openrcm_config.h"

#include "opal/mca/mca.h"

#include "runtime/runtime.h"

/* module functions */
typedef int (*orcm_cfgi_module_init_fn_t)(void);
typedef void (*orcm_cfgi_module_read_config_fn_t)(orcm_spawn_fn_t spawn_apps);
typedef int (*orcm_cfgi_module_finalize_fn_t)(void);

/* component struct */
typedef struct {
    /** Base component description */
    mca_base_component_t cfgic_version;
    /** Base component data block */
    mca_base_component_data_t cfgic_data;
} orcm_cfgi_base_component_2_0_0_t;
/** Convenience typedef */
typedef orcm_cfgi_base_component_2_0_0_t orcm_cfgi_base_component_t;

/* module struct */
typedef struct {
    orcm_cfgi_module_init_fn_t        init;
    orcm_cfgi_module_read_config_fn_t read_config;
    orcm_cfgi_module_finalize_fn_t    finalize;
} orcm_cfgi_base_module_t;

/** Interface for LEADER selection */
ORCM_DECLSPEC extern orcm_cfgi_base_module_t orcm_cfgi;

/*
 * Macro for use in components that are of type coll
 */
#define ORCM_CFGI_BASE_VERSION_2_0_0 \
  MCA_BASE_VERSION_2_0_0, \
  "cfgi", 2, 0, 0

#endif /* ORCM_CFGI_H */

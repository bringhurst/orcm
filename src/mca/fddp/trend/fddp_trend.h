/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 *
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */
/**
 * @file
 *
 */
#ifndef ORCM_FDDP_TREND_H
#define ORCM_FDDP_TREND_H

#include "openrcm_config.h"

#include "mca/fddp/fddp.h"

BEGIN_C_DECLS

struct orcm_fddp_trend_component_t {
    orcm_fddp_base_component_t super;
    int window_size;
};
typedef struct orcm_fddp_trend_component_t orcm_fddp_trend_component_t;

ORCM_MODULE_DECLSPEC extern orcm_fddp_trend_component_t mca_fddp_trend_component;
extern orcm_fddp_base_module_t orcm_fddp_trend_module;


END_C_DECLS

#endif

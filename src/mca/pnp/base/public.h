/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef PNP_BASE_PUBLIC_H
#define PNP_BASE_PUBLIC_H
#include "openrcm_config_private.h"

#include "mca/pnp/pnp.h"

BEGIN_C_DECLS

typedef struct {
    int output;
    opal_list_t opened;
    int max_msgs;
} orcm_pnp_base_t;
ORCM_DECLSPEC extern orcm_pnp_base_t orcm_pnp_base;

ORCM_DECLSPEC int orcm_pnp_base_open(void);
ORCM_DECLSPEC int orcm_pnp_base_select(void);
ORCM_DECLSPEC int orcm_pnp_base_close(void);

ORCM_DECLSPEC extern const mca_base_component_t *orcm_pnp_base_components[];

END_C_DECLS

#endif

/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef PNP_DEFAULT_H
#define PNP_DEFAULT_H

#include "openrcm.h"

#include "opal/class/opal_list.h"

#include "mca/pnp/pnp_types.h"

/* Functions in the pnp default component */

int orcm_pnp_default_component_open(void);
int orcm_pnp_default_component_close(void);
int orcm_pnp_default_component_query(mca_base_module_2_0_0_t **module, int *priority);
int orcm_pnp_default_component_register(void);

ORCM_DECLSPEC extern orcm_pnp_base_component_t mca_orcm_pnp_default_component;
ORCM_DECLSPEC extern orcm_pnp_base_module_t orcm_pnp_default_module;

#endif /* PNP_DEFAULT_H */

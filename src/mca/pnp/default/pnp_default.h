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

/* Functions in the pnp default component */

int orcm_pnp_default_component_open(void);
int orcm_pnp_default_component_close(void);
int orcm_pnp_default_component_query(mca_base_module_2_0_0_t **module, int *priority);
int orcm_pnp_default_component_register(void);

extern orcm_pnp_base_component_t mca_pnp_default_component;
extern orcm_pnp_base_module_t orcm_pnp_default_module;

#endif /* PNP_DEFAULT_H */

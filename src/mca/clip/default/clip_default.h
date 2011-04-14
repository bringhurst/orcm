/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef CLIP_DEFAULT_H
#define CLIP_DEFAULT_H

#include "openrcm.h"

/* Functions in the clip default component */

int orcm_clip_default_component_open(void);
int orcm_clip_default_component_close(void);
int orcm_clip_default_component_query(mca_base_module_2_0_0_t **module, int *priority);
int orcm_clip_default_component_register(void);

ORCM_DECLSPEC extern orcm_clip_base_component_t mca_orcm_clip_default_component;
ORCM_DECLSPEC extern orcm_clip_base_module_t orcm_clip_default_module;

#endif /* CLIP_DEFAULT_H */

/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef CLIP_BASE_PUBLIC_H
#define CLIP_BASE_PUBLIC_H

#include "openrcm.h"

#include "mca/clip/clip.h"

/*
 * globals that might be needed
 */
typedef struct {
    int output;
    opal_list_t opened;
} orcm_clip_base_t;

ORCM_DECLSPEC extern orcm_clip_base_t orcm_clip_base;

int orcm_clip_base_open(void);
int orcm_clip_base_select(void);
int orcm_clip_base_close(void);

extern const mca_base_component_t *orcm_clip_base_components[];

#endif

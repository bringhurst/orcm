/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef LEADER_FIFL_H
#define LEADER_FIFL_H

#include "openrcm.h"

/* Functions in the pnp default component */

typedef struct {
    orcm_leader_base_component_t super;
    int trigger;
} orcm_leader_fifl_component_t;

extern orcm_leader_fifl_component_t mca_orcm_leader_fifl_component;
extern orcm_leader_base_module_t orcm_leader_fifl_module;

#endif /* LEADER_FIFL_H */

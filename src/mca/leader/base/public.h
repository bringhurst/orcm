/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef LEADER_BASE_PUBLIC_H
#define LEADER_BASE_PUBLIC_H

#include "openrcm_config.h"

#include "mca/leader/leader.h"

int orcm_leader_base_open(void);
int orcm_leader_base_select(void);
int orcm_leader_base_close(void);

extern const mca_base_component_t *orcm_leader_base_components[];

#endif

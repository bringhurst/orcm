/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef LEADER_BASE_PRIVATE_H
#define LEADER_BASE_PRIVATE_H

#include "openrcm.h"

#include "opal/class/opal_list.h"

#include "orte/runtime/orte_globals.h"

#include "mca/leader/leader_types.h"

BEGIN_C_DECLS

/*
 * globals that might be needed
 */
typedef struct {
    int output;
    opal_list_t opened;
} orcm_leader_base_t;

ORCM_DECLSPEC extern orcm_leader_base_t orcm_leader_base;

typedef struct {
    opal_list_item_t super;
    char *stringid;
    orte_proc_t *leader;
    orcm_leader_cbfunc_t cbfunc;
} orcm_leader_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_leader_t);

END_C_DECLS

#endif

/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef ORCM_LEADER_TYPES_H
#define ORCM_LEADER_TYPES_H

#include "openrcm.h"

#include "orte/types.h"

#define ORCM_LEADER_WILDCARD    ORTE_VPID_WILDCARD

typedef void (*orcm_leader_cbfunc_t)(char *app,
                                     char *version,
                                     char *release,
                                     int sibling);
#endif /* ORCM_LEADER_TYPES_H */

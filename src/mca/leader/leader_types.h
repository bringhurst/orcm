/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef ORCM_LEADER_TYPES_H
#define ORCM_LEADER_TYPES_H

#include "openrcm_config.h"


#define ORCM_LEADER_WILDCARD    -1

typedef void (*orcm_leader_cbfunc_t)(char *app,
                                     char *version,
                                     char *release,
                                     int sibling);
#endif /* ORCM_LEADER_TYPES_H */

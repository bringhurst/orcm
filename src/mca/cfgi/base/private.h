/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef CFGI_BASE_PRIVATE_H
#define CFGI_BASE_PRIVATE_H

#include "openrcm_config_private.h"
#include "include/constants.h"

#include "orte/runtime/orte_globals.h"

BEGIN_C_DECLS
void orcm_cfgi_base_spawn_app(char *cmd, bool add_procs, bool continuous, bool debug,
                              int restarts, int np, char *hosts, bool constrain);

int orcm_cfgi_base_kill_app(orte_job_t *jdata, char *replicas);

END_C_DECLS

#endif

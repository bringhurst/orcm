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

#include "opal/class/opal_list.h"
#include "opal/dss/dss_types.h"

#include "orte/runtime/orte_globals.h"

#include "mca/cfgi/cfgi.h"

BEGIN_C_DECLS

ORCM_DECLSPEC int orcm_cfgi_base_spawn_app(orte_job_t *jdata);

ORCM_DECLSPEC int orcm_cfgi_base_kill_app(opal_buffer_t *buffer);

ORCM_DECLSPEC int orcm_cfgi_base_check_job(orte_job_t *jdat);

typedef struct {
    opal_list_item_t super;
    orcm_cfgi_base_module_t *module;
} orcm_cfgi_base_selected_module_t;
OBJ_CLASS_DECLARATION(orcm_cfgi_base_selected_module_t);

END_C_DECLS

#endif

/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef CFGI_BASE_PUBLIC_H
#define CFGI_BASE_PUBLIC_H

#include "openrcm_config_private.h"
#include "include/constants.h"

#include "opal/class/opal_list.h"
#include "opal/class/opal_pointer_array.h"

#include "orte/threads/threads.h"
#include "orte/runtime/orte_globals.h"

#include "mca/cfgi/cfgi.h"

/*
 * globals that might be needed
 */
typedef struct {
    int output;
    int num_active_apps;
    orte_job_t *daemons;
    orte_thread_ctl_t ctl;
    opal_pointer_array_t installed_apps;
    opal_pointer_array_t confgd_apps;
    int launch_pipe[2];
    opal_event_t launch_event;
} orcm_cfgi_base_t;

ORCM_DECLSPEC extern orcm_cfgi_base_t orcm_cfgi_base;

ORCM_DECLSPEC int orcm_cfgi_base_open(void);
ORCM_DECLSPEC int orcm_cfgi_base_select(void);
ORCM_DECLSPEC int orcm_cfgi_base_close(void);
ORCM_DECLSPEC void orcm_cfgi_base_activate(void);

ORCM_DECLSPEC extern const mca_base_component_t *orcm_cfgi_base_components[];
ORCM_DECLSPEC extern opal_list_t orcm_cfgi_components_available;
ORCM_DECLSPEC extern opal_list_t orcm_cfgi_selected_modules;

#endif

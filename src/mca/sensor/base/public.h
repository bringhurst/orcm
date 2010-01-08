/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef MCA_SENSOR_BASE_H
#define MCA_SENSOR_BASE_H

/*
 * includes
 */
#include "openrcm.h"

#include "opal/class/opal_list.h"
#include "opal/mca/mca.h"

#include "mca/sensor/sensor.h"


/*
 * Global functions for MCA overall collective open and close
 */
BEGIN_C_DECLS

/*
 * function definitions
 */
ORCM_DECLSPEC    int orcm_sensor_base_open(void);
ORCM_DECLSPEC    int orcm_sensor_base_select(void);
ORCM_DECLSPEC    int orcm_sensor_base_close(void);

/*
 * globals that might be needed
 */

ORCM_DECLSPEC extern int orcm_sensor_base_output;
ORCM_DECLSPEC extern bool orcm_sensor_initialized;
ORCM_DECLSPEC extern opal_list_t mca_sensor_base_components_available;
ORCM_DECLSPEC extern opal_list_t orcm_sensor_base_selected_modules;

/* object definition */
typedef struct {
    opal_list_item_t super;
    orcm_sensor_base_component_t    *component;
    orcm_sensor_base_module_t       *module;
} orcm_sensor_base_selected_pair_t;
OBJ_CLASS_DECLARATION(orcm_sensor_base_selected_pair_t);

END_C_DECLS
#endif

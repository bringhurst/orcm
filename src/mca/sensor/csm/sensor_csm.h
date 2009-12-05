/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 *
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */
/**
 * @file
 *
 * Shared memory-mapped sensors
 */
#ifndef ORCM_SENSOR_CSM_H
#define ORCM_SENSOR_CSM_H

#include "openrcm_config.h"

#include "mca/sensor/sensor.h"

BEGIN_C_DECLS

struct orcm_sensor_csm_component_t {
    orcm_sensor_base_component_t super;
    int sample_rate;
    int celsius_limit;
};
typedef struct orcm_sensor_csm_component_t orcm_sensor_csm_component_t;

ORCM_MODULE_DECLSPEC extern orcm_sensor_csm_component_t mca_sensor_csm_component;
extern orcm_sensor_base_module_t orcm_sensor_csm_module;


END_C_DECLS

#endif

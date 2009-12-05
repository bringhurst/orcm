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
 * Process Resource Utilization sensor 
 */
#ifndef ORCM_SENSOR_PRU_H
#define ORCM_SENSOR_PRU_H

#include "openrcm_config.h"

#include "mca/sensor/sensor.h"

BEGIN_C_DECLS

struct orcm_sensor_pru_component_t {
    orcm_sensor_base_component_t super;
    int sample_rate;
    uint64_t memory_limit;
};
typedef struct orcm_sensor_pru_component_t orcm_sensor_pru_component_t;

ORCM_MODULE_DECLSPEC extern orcm_sensor_pru_component_t mca_sensor_pru_component;
extern orcm_sensor_base_module_t orcm_sensor_pru_module;


END_C_DECLS

#endif

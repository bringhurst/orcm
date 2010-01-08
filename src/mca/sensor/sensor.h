/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * @file:
 *
 */

#ifndef MCA_SENSOR_H
#define MCA_SENSOR_H

/*
 * includes
 */

#include "openrcm.h"

#include "opal/mca/mca.h"

BEGIN_C_DECLS

/*
 * Component functions - all MUST be provided!
 */

/* initialize the selected module */
typedef int (*orcm_sensor_base_module_init_fn_t)(void);
    
/* finalize the selected module */
typedef void (*orcm_sensor_base_module_finalize_fn_t)(void);

/* start collecting data */
typedef void (*orcm_sensor_base_module_start_fn_t)(void);

/* stop collecting data */
typedef void (*orcm_sensor_base_module_stop_fn_t)(void);

/* API module */
/*
 * Ver 1.0
 */
struct orcm_sensor_base_API_module_1_0_0_t {
    orcm_sensor_base_module_start_fn_t      start;
    orcm_sensor_base_module_stop_fn_t       stop;
};

typedef struct orcm_sensor_base_API_module_1_0_0_t orcm_sensor_base_API_module_1_0_0_t;
typedef orcm_sensor_base_API_module_1_0_0_t orcm_sensor_base_API_module_t;


/*
 * Component modules Ver 1.0
 */
struct orcm_sensor_base_module_1_0_0_t {
    orcm_sensor_base_module_init_fn_t       init;
    orcm_sensor_base_module_finalize_fn_t   finalize;
    orcm_sensor_base_module_start_fn_t      start;
    orcm_sensor_base_module_stop_fn_t       stop;
};

typedef struct orcm_sensor_base_module_1_0_0_t orcm_sensor_base_module_1_0_0_t;
typedef orcm_sensor_base_module_1_0_0_t orcm_sensor_base_module_t;

/*
 * the standard component data structure
 */
struct orcm_sensor_base_component_1_0_0_t {
    mca_base_component_t base_version;
    mca_base_component_data_t base_data;
};
typedef struct orcm_sensor_base_component_1_0_0_t orcm_sensor_base_component_1_0_0_t;
typedef orcm_sensor_base_component_1_0_0_t orcm_sensor_base_component_t;



/*
 * Macro for use in components that are of type sensor v1.0.0
 */
#define ORCM_SENSOR_BASE_VERSION_1_0_0 \
  /* sensor v1.0 is chained to MCA v2.0 */ \
  MCA_BASE_VERSION_2_0_0, \
  /* sensor v1.0 */ \
  "sensor", 1, 0, 0

/* Global structure for accessing sensor functions
 */
ORCM_DECLSPEC extern orcm_sensor_base_API_module_t orcm_sensor;  /* holds API function pointers */

END_C_DECLS

#endif /* MCA_SENSOR_H */

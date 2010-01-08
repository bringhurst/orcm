/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#ifndef MCA_FDDP_H
#define MCA_FDDP_H

/*
 * includes
 */

#include "openrcm.h"

#include "opal/mca/mca.h"

#include "mca/sensor/sensor_types.h"

BEGIN_C_DECLS

/*
 * Component functions - all MUST be provided!
 */

/* initialize the selected module */
typedef int (*orcm_fddp_base_module_init_fn_t)(void);
    
/* finalize the selected module */
typedef int (*orcm_fddp_base_module_finalize_fn_t)(void);

typedef int (*orcm_fddp_base_module_process_fn_t)(orcm_sensor_data_t *data,
                                                  int num_bins, uint8_t *failure_likelihood);
    
/*
 * Ver 1.0
 */
struct orcm_fddp_base_module_1_0_0_t {
    orcm_fddp_base_module_init_fn_t       init;
    orcm_fddp_base_module_finalize_fn_t   finalize;
    orcm_fddp_base_module_process_fn_t    process;
};

typedef struct orcm_fddp_base_module_1_0_0_t orcm_fddp_base_module_1_0_0_t;
typedef orcm_fddp_base_module_1_0_0_t orcm_fddp_base_module_t;

/*
 * the standard component data structure
 */
struct orcm_fddp_base_component_1_0_0_t {
    mca_base_component_t base_version;
    mca_base_component_data_t base_data;
};
typedef struct orcm_fddp_base_component_1_0_0_t orcm_fddp_base_component_1_0_0_t;
typedef orcm_fddp_base_component_1_0_0_t orcm_fddp_base_component_t;



/*
 * Macro for use in components that are of type fddp v1.0.0
 */
#define ORCM_FDDP_BASE_VERSION_1_0_0 \
  /* fddp v1.0 is chained to MCA v2.0 */ \
  MCA_BASE_VERSION_2_0_0, \
  /* fddp v1.0 */ \
  "fddp", 1, 0, 0

/* Global structure for accessing fddp functions
 */
ORCM_DECLSPEC extern orcm_fddp_base_module_t orcm_fddp;  /* holds selected module's function pointers */

END_C_DECLS

#endif /* MCA_FDDP_H */

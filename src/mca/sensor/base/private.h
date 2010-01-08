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

#ifndef MCA_SENSOR_PRIVATE_H
#define MCA_SENSOR_PRIVATE_H

/*
 * includes
 */
#include "openrcm.h"

#include "opal/dss/dss_types.h"

#include "mca/sensor/sensor_types.h"


/*
 * Global functions for MCA overall collective open and close
 */
BEGIN_C_DECLS

/*
 * function definitions
 */
ORCM_DECLSPEC    int orcm_sensor_scale_data(orcm_sensor_data_t *target, int num_values, float *data);

END_C_DECLS
#endif

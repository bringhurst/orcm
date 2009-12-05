/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "openrcm_config.h"
#include "constants.h"

#include "opal/mca/mca.h"
#include "opal/util/output.h"
#include "opal/mca/base/base.h"
#include "opal/mca/base/mca_base_param.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "mca/sensor/base/private.h"


int orcm_sensor_scale_data(orcm_sensor_data_t *target, int num_values, float *data)
{
    int i;
    
    /* ensure we have enough data storage in the sensor data object */
    if (NULL != target->data.bytes) {
        /* clear out pre-existing data */
        free(target->data.bytes);
    }
    /* allocate what we need */
    target->data.bytes = (uint8_t*)malloc(num_values * sizeof(uint8_t));
    memset(target->data.bytes, 0, num_values);
    target->data.size = num_values;

    /* convert the data */
    for (i=0; i < num_values; i++) {
        target->data.bytes[i] = UINT8_MAX * (data[i] - target->min) / (target->max - target->min);
    }
    
    return ORCM_SUCCESS;
}

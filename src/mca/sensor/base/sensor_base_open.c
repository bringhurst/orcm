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

#include "mca/sensor/base/public.h"


#include "mca/sensor/base/components.h"


const mca_base_component_t *orcm_sensor_base_components[] = {
/*    &mca_sensor_csm_component.super.base_version, */
    &mca_sensor_pru_component.super.base_version,
    NULL
};

/* object definition */
static void construct(orcm_sensor_base_selected_pair_t *obj)
{
    obj->component = NULL;
    obj->module = NULL;
}

static void destruct(orcm_sensor_base_selected_pair_t *obj)
{
    if (NULL != obj->module->finalize) {
        obj->module->finalize();
    }
}

OBJ_CLASS_INSTANCE(orcm_sensor_base_selected_pair_t,
                   opal_list_item_t,
                   construct, destruct);

/* base functions */
static void start(void);
static void stop(void);

/*
 * Global variables
 */
int orcm_sensor_base_output = -1;
orcm_sensor_base_API_module_t orcm_sensor = {
    start,
    stop
};
opal_list_t mca_sensor_base_components_available;
opal_list_t orcm_sensor_base_selected_modules;

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
int orcm_sensor_base_open(void)
{
    /* Debugging / verbose output.  Always have stream open, with
       verbose set by the mca open system... */
    orcm_sensor_base_output = opal_output_open(NULL);
    
    /* construct the list of modules */
    OBJ_CONSTRUCT(&orcm_sensor_base_selected_modules, opal_list_t);
    
    /* Open up all available components */

    if (ORCM_SUCCESS !=
        mca_base_components_open("sensor", orcm_sensor_base_output,
                                 orcm_sensor_base_components,
                                 &mca_sensor_base_components_available, true)) {
        return ORCM_ERROR;
    }

    /* All done */

    return ORCM_SUCCESS;
}

static void start(void)
{
    orcm_sensor_base_selected_pair_t *pair;
    opal_list_item_t *item;
    
    for (item = opal_list_get_first(&orcm_sensor_base_selected_modules);
         opal_list_get_end(&orcm_sensor_base_selected_modules) != item;
         item = opal_list_get_next(item)) {
        pair = (orcm_sensor_base_selected_pair_t*)item;
        if (NULL != pair->module->start) {
            pair->module->start();
        }
    }
    return;    
}

static void stop(void)
{
    orcm_sensor_base_selected_pair_t *pair;
    opal_list_item_t *item;
    
    for (item = opal_list_get_first(&orcm_sensor_base_selected_modules);
         opal_list_get_end(&orcm_sensor_base_selected_modules) != item;
         item = opal_list_get_next(item)) {
        pair = (orcm_sensor_base_selected_pair_t*)item;
        if (NULL != pair->module->stop) {
            pair->module->stop();
        }
    }
    return;
}

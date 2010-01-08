/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config_private.h"
#include "constants.h"

#include "opal/mca/base/base.h"
#include "opal/mca/base/mca_base_param.h"
#include "opal/class/opal_pointer_array.h"

#include "orte/util/proc_info.h"
#include "orte/util/show_help.h"

#include "sensor_csm.h"

/*
 * Local functions
 */

static int orcm_sensor_csm_open(void);
static int orcm_sensor_csm_close(void);
static int orcm_sensor_csm_query(mca_base_module_t **module, int *priority);

orcm_sensor_csm_component_t mca_sensor_csm_component = {
    {
        {
            ORCM_SENSOR_BASE_VERSION_1_0_0,
            
            "csm", /* MCA component name */
            ORCM_MAJOR_VERSION,  /* MCA component major version */
            ORCM_MINOR_VERSION,  /* MCA component minor version */
            ORCM_RELEASE_VERSION,  /* MCA component release version */
            orcm_sensor_csm_open,  /* component open  */
            orcm_sensor_csm_close, /* component close */
            orcm_sensor_csm_query  /* component query */
        },
        {
            /* The component is checkpoint ready */
            MCA_BASE_METADATA_PARAM_CHECKPOINT
        }
    }
};


/**
  * component open/close/init function
  */
static int orcm_sensor_csm_open(void)
{
    mca_base_component_t *c = &mca_sensor_csm_component.super.base_version;

    /* lookup parameters */
    mca_base_param_reg_int(c, "sample_rate",
                              "Sample rate in seconds (default=10)",
                              false, false, 10,  &mca_sensor_csm_component.sample_rate);
    
    mca_base_param_reg_int(c, "celsius_limit",
                           "Max temperature in celsius (default=50)",
                           false, false, 50,  &mca_sensor_csm_component.celsius_limit);
    
    return ORCM_SUCCESS;
}


static int orcm_sensor_csm_query(mca_base_module_t **module, int *priority)
{    
    *priority = 0;  /* select only if specified */
    *module = (mca_base_module_t *)&orcm_sensor_csm_module;
    
    return ORCM_SUCCESS;
}

/**
 *  Close all subsystems.
 */

static int orcm_sensor_csm_close(void)
{
    return ORCM_SUCCESS;
}

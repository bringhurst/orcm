/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config_private.h"
#include "include/constants.h"

#include "opal/util/error.h"

#include "orte/runtime/runtime.h"
#include "orte/mca/errmgr/errmgr.h"

#include "mca/pnp/base/public.h"
#include "mca/fddp/base/public.h"
#include "mca/sensor/base/public.h"
#include "runtime/runtime.h"

const char openrcm_version_string[] = "OPENRCM 0.1";
bool orcm_initialized = false;
bool orcm_util_initialized = false;
bool orcm_finalizing = false;
int orcm_debug_output = -1;
int orcm_debug_verbosity = 0;

int orcm_init(orcm_proc_type_t flags)
{
    int ret;
    char *error;
    int spin;
    
    if (NULL != getenv("ORCM_MCA_spin")) {
        spin = 1;
        /* spin until a debugger can attach */
        while (0 != spin) {
            ret = 0;
            while (ret < 10000) {
                ret++;
            };
        }
    }
    
    if (!orcm_util_initialized) {
        orcm_init_util();
    }
    
    /* set some envars generally needed */
    putenv("OMPI_MCA_routed=cm");
    
    if (OPENRCM_MASTER & flags) {
        /* add envars the master needs */
        putenv("OMPI_MCA_rmaps=resilient");
        putenv("OMPI_MCA_plm=rsh");
        
        /* if we are the master, then init us
         * with ORTE as the HNP
         */
        if (ORTE_SUCCESS != (ret = orte_init(NULL, NULL, ORTE_PROC_HNP))) {
            error = "orte_init";
            goto error;
        }
        
    }  else if (OPENRCM_DAEMON & flags) {
        /* ensure we use the right ess module if one isn't given */
        if (NULL == getenv("OMPI_MCA_ess=cm")) {
            putenv("OMPI_MCA_ess=cm");
        }
        if (ORTE_SUCCESS != (ret = orte_init(NULL, NULL, ORTE_PROC_DAEMON))) {
            error = "orte_init";
            goto error;
        }

    } else if (OPENRCM_TOOL & flags) {
        /* tools start independently, so we have to
         * ensure they get the correct ess module
         */
        putenv("OMPI_MCA_ess=cm");
        if (ORTE_SUCCESS != (ret = orte_init(NULL, NULL, ORTE_PROC_TOOL))) {
            error = "orte_init";
            goto error;
        }
        
    } else if (OPENRCM_APP & flags) {
        /* apps are always started by the daemon, so
         * they will be told the right components to open
         */
        if (ORTE_SUCCESS != (ret = orte_init(NULL, NULL, ORTE_PROC_NON_MPI))) {
            error = "orte_init";
            goto error;
        }
        
        /* setup the pnp framework */
        if (ORCM_SUCCESS != (ret = orcm_pnp_base_open())) {
            error = "pnp_open";
            goto error;
        }
        if (ORCM_SUCCESS != (ret = orcm_pnp_base_select())) {
            error = "pnp_select";
            goto error;
        }
        /* setup the leader framework */
        if (ORCM_SUCCESS != (ret = orcm_leader_base_open())) {
            error = "pnp_open";
            goto error;
        }
        if (ORCM_SUCCESS != (ret = orcm_leader_base_select())) {
            error = "pnp_select";
            goto error;
        }
    } else {
        error = "unknown flag";
        ret = ORTE_ERR_FATAL;
        goto error;
    }
    
    if (ORTE_PROC_IS_HNP || ORTE_PROC_IS_DAEMON) {
        /* setup the sensors */
        if (ORTE_SUCCESS != (ret = orcm_sensor_base_open())) {
            ORTE_ERROR_LOG(ret);
            error = "orcm_sensor_open";
            goto error;
        }
        if (ORTE_SUCCESS != (ret = orcm_sensor_base_select())) {
            ORTE_ERROR_LOG(ret);
            error = "orcm_sensor_select";
            goto error;
        }
        
        /* setup the fddp */
        if (ORTE_SUCCESS != (ret = orcm_fddp_base_open())) {
            ORTE_ERROR_LOG(ret);
            error = "orcm_sensor_open";
            goto error;
        }
        if (ORTE_SUCCESS != (ret = orcm_fddp_base_select())) {
            ORTE_ERROR_LOG(ret);
            error = "orcm_sensor_select";
            goto error;
        }
    }
    
    orcm_initialized = true;
    
    return ORCM_SUCCESS;

error:
    if (ORCM_ERR_SILENT != ret) {
        orte_show_help("help-openrcm-runtime.txt",
                       "orcm_init:startup:internal-failure",
                       true, error, ORTE_ERROR_NAME(ret), ret);
    }
    
    return ret;
}

int orcm_init_util(void)
{
    int ret;
    char *error;
    
    /* Ensure that enough of OPAL is setup for us to be able to run */
    if( ORTE_SUCCESS != (ret = opal_init_util(NULL, NULL)) ) {
        error = "opal_init_util";
        goto error;
    }
    /* register handler for errnum -> string conversion */
    opal_error_register("OPENRCM", ORCM_ERR_BASE, ORCM_ERR_MAX, orcm_err2str);
    /* register where the OPENRCM show_help files are located */
    if (ORTE_SUCCESS != (ret = opal_show_help_add_dir(OPENRCM_HELPFILES))) {
        error = "register show_help_dir";
    goto error;
    }
    
    orcm_util_initialized = true;
    
    return ORCM_SUCCESS;
    
error:
    if (ORCM_ERR_SILENT != ret) {
        orte_show_help("help-openrcm-runtime.txt",
                       "orcm_init:startup:internal-failure",
                       true, error, ORTE_ERROR_NAME(ret), ret);
    }
    
    return ret;
}


/**   INSTANTIATE OPENRCM OBJECTS **/
static void spawn_construct(orcm_spawn_event_t *ptr)
{
    ptr->ev = (opal_event_t*)malloc(sizeof(opal_event_t));
    ptr->cmd = NULL;
    ptr->np = 0;
    ptr->hosts = NULL;
    ptr->constrain = false;
    ptr->add_procs = false;
    ptr->debug = false;
}
static void spawn_destruct(orcm_spawn_event_t *ptr)
{
    if (NULL != ptr->ev) { 
        free(ptr->ev); 
    } 
    if (NULL != ptr->cmd) {
        free(ptr->cmd);
    }
    if (NULL != ptr->hosts) {
        free(ptr->hosts);
    }
}
OBJ_CLASS_INSTANCE(orcm_spawn_event_t,
                   opal_object_t,
                   spawn_construct,
                   spawn_destruct);

static void orcm_sensor_data_construct(orcm_sensor_data_t *ptr)
{
    ptr->sensor = NULL;
    ptr->scaling_law = ORCM_SENSOR_SCALE_LINEAR;
    ptr->min = 0.0;
    ptr->max = 100.0;
    ptr->gain = 1.0;
    ptr->data.size = 0;
    ptr->data.bytes = NULL;
}
static void orcm_sensor_data_destruct(orcm_sensor_data_t *ptr)
{
    if (NULL != ptr->sensor) {
        free(ptr->sensor);
    }
    if (NULL != ptr->data.bytes) {
        free(ptr->data.bytes);
    }
}
OBJ_CLASS_INSTANCE(orcm_sensor_data_t,
                   opal_object_t,
                   orcm_sensor_data_construct,
                   orcm_sensor_data_destruct);


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

#include "opal/util/output.h"
#include "opal/util/os_dirpath.h"

#include "runtime/runtime.h"

#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/file/cfgi_file.h"

orcm_cfgi_file_component_t mca_orcm_cfgi_file_component = {
    {
        /* First, the mca_base_component_t struct containing meta
         information about the component itself */

        {
            ORCM_CFGI_BASE_VERSION_2_0_0,

            "file",
            OPENRCM_MAJOR_VERSION,
            OPENRCM_MINOR_VERSION,
            OPENRCM_RELEASE_VERSION,
            orcm_cfgi_file_component_open,
            orcm_cfgi_file_component_close,
            orcm_cfgi_file_component_query,
            orcm_cfgi_file_component_register
        },
        {
            /* The component is checkpoint ready */
            MCA_BASE_METADATA_PARAM_CHECKPOINT
        }
    }
};

int orcm_cfgi_file_component_open(void)
{
    mca_base_component_t *c = &mca_orcm_cfgi_file_component.super.cfgic_version;

    /* check for file name */
    mca_base_param_reg_string(c, "config_dir",
                              "Directory containing the configuration snippets [default: /etc/orcm]",
                              false, false, "/etc/orcm", &mca_orcm_cfgi_file_component.dir);

    mca_base_param_reg_int(c, "ping_rate",
                              "How often to check the config directory for changes, in seconds [default: 10]",
                              false, false, 10, &mca_orcm_cfgi_file_component.rate);

    return ORCM_SUCCESS;
}

int orcm_cfgi_file_component_close(void)
{
    return ORCM_SUCCESS;
}

int orcm_cfgi_file_component_query(mca_base_module_t **module, int *priority)
{
    if (ORCM_PROC_IS_SCHEDULER &&
        NULL != mca_orcm_cfgi_file_component.dir) {
#ifndef HAVE_SYS_INOTIFY_H
        if (0 < mca_orcm_cfgi_file_component.rate) {
#endif
            *module = (mca_base_module_t*)&orcm_cfgi_file_module;
            *priority = 100;
            return ORCM_SUCCESS;
#ifndef HAVE_SYS_INOTIFY_H
        }
#endif
    }

    *module = NULL;
    *priority = 0;
    return ORCM_ERROR;
}

int orcm_cfgi_file_component_register(void)
{
    return ORCM_SUCCESS;
}


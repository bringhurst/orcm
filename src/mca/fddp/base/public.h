/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef MCA_FDDP_BASE_H
#define MCA_FDDP_BASE_H

/*
 * includes
 */
#include "openrcm_config.h"

#include "opal/class/opal_list.h"
#include "opal/mca/mca.h"

#include "mca/fddp/fddp.h"


/*
 * Global functions for MCA overall collective open and close
 */
BEGIN_C_DECLS

/*
 * function definitions
 */
ORCM_DECLSPEC    int orcm_fddp_base_open(void);
ORCM_DECLSPEC    int orcm_fddp_base_select(void);
ORCM_DECLSPEC    int orcm_fddp_base_close(void);

/*
 * globals that might be needed
 */

ORCM_DECLSPEC extern int orcm_fddp_base_output;
ORCM_DECLSPEC extern bool mca_fddp_base_selected;
ORCM_DECLSPEC extern opal_list_t mca_fddp_base_components_available;
ORCM_DECLSPEC extern orcm_fddp_base_component_t mca_fddp_base_selected_component;

END_C_DECLS
#endif

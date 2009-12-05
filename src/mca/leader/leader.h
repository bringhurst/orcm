/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef ORCM_LEADER_H
#define ORCM_LEADER_H

#include "openrcm_config.h"

#include "opal/mca/mca.h"
#include "opal/class/opal_list.h"
#include "opal/dss/dss_types.h"

#include "orte/types.h"

#include "mca/pnp/pnp_types.h"

/* module functions */
typedef int (*orcm_leader_module_init_fn_t)(void);
typedef int (*orcm_leader_module_finalize_fn_t)(void);
typedef bool (*orcm_leader_module_has_leader_failed_fn_t)(orcm_pnp_group_t *grp);

/*
 * Given a list of members of an application group, select a leader
 * who will provide the "official" input. The list will be composed
 * of orcm_pnp_source_t objects
 */
typedef int (*orcm_leader_module_set_leader_fn_t)(orcm_pnp_group_t *grp,
                                                  orte_process_name_t *ldr);

/* component struct */
typedef struct {
    /** Base component description */
    mca_base_component_t leaderc_version;
    /** Base component data block */
    mca_base_component_data_t leaderc_data;
} orcm_leader_base_component_2_0_0_t;
/** Convenience typedef */
typedef orcm_leader_base_component_2_0_0_t orcm_leader_base_component_t;

/* module struct */
typedef struct {
    orcm_leader_module_init_fn_t                init;
    orcm_leader_module_set_leader_fn_t          set_leader;
    orcm_leader_module_has_leader_failed_fn_t   has_leader_failed;
    orcm_leader_module_finalize_fn_t            finalize;
} orcm_leader_base_module_t;

/** Interface for LEADER selection */
ORCM_DECLSPEC extern orcm_leader_base_module_t orcm_leader;

/*
 * Macro for use in components that are of type coll
 */
#define ORCM_LEADER_BASE_VERSION_2_0_0 \
  MCA_BASE_VERSION_2_0_0, \
  "leader", 2, 0, 0

#endif /* ORCM_LEADER_H */

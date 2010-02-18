/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef ORCM_LEADER_H
#define ORCM_LEADER_H

#include "openrcm.h"

#include "opal/mca/mca.h"
#include "opal/class/opal_list.h"
#include "opal/dss/dss_types.h"

#include "orte/types.h"

#include "mca/pnp/pnp_types.h"
#include "mca/leader/leader_types.h"

/* module functions */

/* initialize the module - called only from within the
 * framework setup code
 */
typedef int (*orcm_leader_module_init_fn_t)(void);

/* finalize the module - called only from within the
 * framework setup code
 */
typedef int (*orcm_leader_module_finalize_fn_t)(void);

/* check to see if the current leader has failed. The pnp
 * code will call this function upon receipt of each message
 * to see if the current leader needs to be replaced. It is
 * up to the individual leader module to use whatever algo
 * it wants to make this determination
 */
typedef bool (*orcm_leader_module_has_leader_failed_fn_t)(orcm_pnp_group_t *grp,
                                                          orcm_pnp_source_t *src);

/* Manually set the leader for a given application triplet. A value
 * of ORCM_LEADER_WILDCARD will cause data from all siblings to be
 * passed through as if they all are leaders.
 *
 * NOTE: A value beyond the range of available siblings will result
 * in no data being passed through at all!
 *
 * NOTE: once manually set, the system will not auto-select a new
 * leader. Instead, the provided cbfunc will be called when the
 * selected leader is determined to have failed. If a NULL is
 * given for the cbfunc, then the system WILL auto-select a new
 * leader upon failure of the specified one.
 *
 * If ORCM_LEADER_WILDCARD and a cbfunc are provided, then the
 * cbfunc will be called whenever any sibling fails.
 */
typedef int (*orcm_leader_module_set_leader_fn_t)(char *app,
                                                  char *version,
                                                  char *release,
                                                  orte_vpid_t sibling,
                                                  orcm_leader_cbfunc_t cbfunc);
/*
 * Given a list of members of an application group, select a leader
 * who will provide the "official" input.
 */
typedef int (*orcm_leader_module_select_leader_fn_t)(orcm_pnp_group_t *grp);

/* Get the current leader of a group */
typedef orcm_pnp_source_t* (*orcm_leader_module_get_leader_fn_t)(orcm_pnp_group_t *grp);

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
    orcm_leader_module_select_leader_fn_t       select_leader;
    orcm_leader_module_has_leader_failed_fn_t   has_leader_failed;
    orcm_leader_module_get_leader_fn_t          get_leader;
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

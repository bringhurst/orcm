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
typedef void (*orcm_leader_module_finalize_fn_t)(void);

/* check to see if the current leader has failed. The pnp
 * code will call this function upon receipt of each message
 * to see if the current leader needs to be replaced. It is
 * up to the individual leader module to use whatever algo
 * it wants to make this determination
 */
typedef bool (*orcm_leader_module_deliver_msg_fn_t)(const char *stringid,
                                                    const orte_process_name_t *src);

/* Manually set the leader for a given application triplet. A value
 * of ORCM_LEADER_WILDCARD will cause data from all siblings to be
 * passed through as if they all are leaders. A value of ORCM_LEADER_INVALID
 * will cause the function to automatically select a "leader" based
 * on its own internal algorithm
 *
 * NOTE: A value beyond the range of available siblings will result
 * in no data being passed through at all!
 *
 * NOTE: If provided, the cbfunc will be called when the
 * selected leader is determined to have failed.
 *
 * If ORCM_LEADER_WILDCARD and a cbfunc are provided, then the
 * cbfunc will be called whenever any member of the specified triplet fails.
 * Providing NULL for all triplet fields will result in all messages from
 * all triplets to be passed thru (except where specified by other set_leader
 * calls) and callbacks whenever ANY process fails.
 *
 * If the caller wants to be notified whenever ANY process fails while
 * allowing the module to automatically select leaders (except where
 * specified by other set_leader calls), then specify NULL for each triplet
 * field and ORCM_LEADER_INVALID for the sibling.
 */
typedef int (*orcm_leader_module_set_leader_fn_t)(const char *app,
                                                  const char *version,
                                                  const char *release,
                                                  const orte_vpid_t sibling,
                                                  orcm_leader_cbfunc_t cbfunc);
/* Get the current leader of a group */
typedef int (*orcm_leader_module_get_leader_fn_t)(const char *app,
                                                  const char *version,
                                                  const char *release,
                                                  orte_process_name_t *leader);

/* Notify the leader module of a process failure */
typedef void (*orcm_leader_module_proc_failed_fn_t)(const char *stringid,
                                                    const orte_process_name_t failed);

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
    orcm_leader_module_finalize_fn_t            finalize;
    orcm_leader_module_deliver_msg_fn_t         deliver_msg;
    orcm_leader_module_set_leader_fn_t          set_leader;
    orcm_leader_module_get_leader_fn_t          get_leader;
    orcm_leader_module_proc_failed_fn_t         proc_failed;
} orcm_leader_base_module_t;

/** Interface for LEADER selection */
ORCM_DECLSPEC extern orcm_leader_base_module_t orcm_leader;

/*
 * Macro for use in components that are of type coll
 */
#define ORCM_LEADER_BASE_VERSION_2_0_0 \
  MCA_BASE_VERSION_2_0_0, \
  "orcm_leader", 2, 0, 0

#endif /* ORCM_LEADER_H */

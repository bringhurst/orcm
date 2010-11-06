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
#include "opal/dss/dss_types.h"

#include "orte/types.h"
#include "orte/mca/rmcast/rmcast_types.h"

#include "runtime/orcm_globals.h"

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
                                                    const orte_process_name_t *src,
                                                    const orte_rmcast_channel_t chan,
                                                    const orte_rmcast_seq_t seq_num);

/* Define the leader policy for a given application triplet. Understanding
 * how this API works requires a brief review of how applications can be
 * started. There are two ways:
 *
 * (a) individual apps can be started with separate launch commands. This
 *     results in each app having its own orte_jobid, and no defined logical
 *     connection between apps exists.
 *
 * (b) groups of apps can be started with a single launch command. For example,
 *     a user may choose to launch a standard configuration of a group "foo"
 *     consisting of n copies of app1, m copies of app2, etc. In this case,
 *     the app group is given a single common orte_jobid so it can be
 *     treated collectively. This allows, for example, a system admin to define
 *     a typical app group for a given functionality, relieving users from
 *     having to define it themselves every time they want to run it.
 *
 * It also must be noted that any number of app groups can contain an executable
 * that declares itself with the same triplet. Defining a "leader" for an app
 * therefore requires that we distinguish between the two cases above - we need
 * to support cross-jobid leaders for case (a), but also allow an app to indicate
 * that it only wants leaders selected from within its own app group.
 *
 * Accordingly, the API works by:
 *
 * (a) if the jobid in the specified policy is ORTE_JOBID_WILDCARD, then
 *     the module will consider sources from ALL announced triplets as
 *     potential leaders [DEFAULT]
 * (b) if the jobid in the specified policy is ORTE_JOBID_INVALID, then
 *     the module will apply its internal algorithms but ONLY consider
 *     sources from the caller's SAME jobid as potential leaders. This
 *     allows you to specify that you want a leader selected from within
 *     your own app group, and is equivalent to just passing your own
 *     jobid as the value
 * (c) if the jobid in the specified policy is neither of the above values,
 *     then the module will apply its internal algorithms but consider only
 *     sources from that jobid.
 *
 * (d) if the vpid in the specified policy is ORTE_VPID_WILDCARD, then the
 *     module will pass thru messages from ALL sources from within the above
 *     jobid restriction [DEFAULT]
 * (e) if the vpid in the specified policy is ORTE_VPID_INVALID, then the module
 *     will use its internal algorithms to select a leader from within the above
 *     jobid restriction.
 * (f) if the vpid in the specified policy is neither of the above values, then
 *     the module will only pass thru messages from that specific process.
 *
 * A few examples may help illustrate this API:
 *
 * (a) providing ORTE_NAME_WILDCARD as the policy. In this case, both the
 *     jobid and vpid are WILDCARD. Thus, messages from ALL sources will
 *     pass thru.
 *
 * NOTE: as this is the default condition, never calling set_policy will
 * result in messages from ALL sources being passed thru.
 *
 * (b) providing a name where the jobid is ORTE_JOBID_WILDCARD, but the vpid
 *     is ORTE_VPID_INVALID. Consider ALL members of this triplet, regardless
 *     of how they were launched, and use your internal algo to select one to
 *     pass thru.
 * (c) providing ORTE_NAME_INVALID as the leader. Since both jobid and vpid
 *     are INVALID, the module will use its internal algo to select a single
 *     sibling in the triplet from within the current jobid to pass thru. This
 *     allows you to request that a leader be internally selected, but restrict
 *     candidates to siblings of this triplet that were co-launched with you.
 *
 * NOTE: A value beyond the range of available siblings will result
 * in no data being passed through at all!
 *
 * If a cbfunc is provided, then the cbfunc will be called whenever the
 * notify flag condition is met. Thus:
 *
 * (a) ORCM_NOTIFY_NONE => don't notify on any failures. This is
 *     the equivalent of providing NULL for the cbfunc.
 * (b) ORCM_NOTIFY_LDR => execute the cbfunc whenever the current
 *     leader fails
 * (c) ORCM_NOTIFY_GRP => execute the cbfunc whenever any member
 *     within the group of sources eligible to become leader fails
 * (d) ORCM_NOTIFY_ANY => execute the cbfunc whenever any member of
 *     the specified triplet fails, regardless of whether or not that
 *     process was eligible for leadership
 */
typedef int (*orcm_leader_module_set_policy_fn_t)(const char *app,
                                                         const char *version,
                                                         const char *release,
                                                         const orte_process_name_t *policy,
                                                         orcm_notify_t notify,
                                                         orcm_leader_cbfunc_t cbfunc);

/* Manually set the leader for a given application triplet. Overrides
 * any defined policy to set a specific leader.
 */
typedef int (*orcm_leader_module_set_leader_fn_t)(const char *app,
                                                  const char *version,
                                                  const char *release,
                                                  const orte_process_name_t *leader);

/* Get the current leader of a triplet */
typedef int (*orcm_leader_module_get_leader_fn_t)(const char *app,
                                                  const char *version,
                                                  const char *release,
                                                  orte_process_name_t *leader);

/* Notify the leader module of a process failure */
typedef void (*orcm_leader_module_proc_failed_fn_t)(const char *stringid,
                                                    const orte_process_name_t *failed);

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
    orcm_leader_module_set_policy_fn_t          set_policy;
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

/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef ORCM_PNP_H
#define ORCM_PNP_H

#include "openrcm.h"

#include "opal/mca/mca.h"
#include "opal/dss/dss_types.h"

#include "orte/types.h"

#include "runtime/orcm_globals.h"

#include "pnp_types.h"

/* module functions */
typedef int (*orcm_pnp_module_init_fn_t)(void);
typedef int (*orcm_pnp_module_finalize_fn_t)(void);
typedef int (*orcm_pnp_module_disable_comm_fn_t)(void);

/*
 * Announce my existence, specifying my application's name, version
 * and release. All three fields MUST be provided or an error will
 * be returned. If non-NULL, the cbfunc will be called whenever a new
 * group announcement is received. The callback will return the
 * app/version/release triplet of the new group, along with its
 * GROUP_OUTPUT channel id.
 */
typedef int (*orcm_pnp_module_announce_fn_t)(const char *app,
                                             const char *version,
                                             const char *release,
                                             orcm_pnp_announce_fn_t cbfunc);

/*
 * Open a channel to another app/version/release triplet. A callback will be
 * made whenever the first announcement is recvd from a process matching the specified
 * triplet (NULL => WILDCARD for that field, will recv callback for each unique
 * triplet that fits), with the GROUP_INPUT channel for that triplet provided
 * in the call to the cbfunc
 *
 * Because triplets can occur multiple times in a system, we need to distinguish
 * which instance of a triplet the caller wants a channel for:
 *
 * (a) ORTE_JOBID_WILDCARD => whenever the first process from every jobid
 *     instance of the triplet is detected.
 * (b) ORTE_JOBID_INVALID => whenever the first process of the triplet
 *     is detected, regardless of jobid
 * (c) specific jobid => first process of the triplet from the specified jobid
 *     is detected
 */
typedef int (*orcm_pnp_module_open_channel_fn_t)(const char *app,
                                                 const char *version,
                                                 const char *release,
                                                 const orte_jobid_t jobid,
                                                 orcm_pnp_open_channel_cbfunc_t cbfunc);

/*
 * Receive messages associated with the specified app/version/release triplet.
 * A channel of GROUP_INPUT  will listen to messages sent TO the specified triplet
 * on their input channel. A channel of GROUP_OUTPUT will listen to messages sent
 * by the specified triplet on their GROUP_OUTPUT channel. A NULL can be provided
 * in any triplet field to act as a wildcard.
 *
 * Note: it is permissable to register a receive against a triplet on a channel
 * other than its GROUP_INPUT or GROUP_OUTPUT (e.g., SYSTEM_CHANNEL). This will
 * result in the caller receiving messages sent by the specified triplet on
 * that channel.
 */
typedef int (*orcm_pnp_module_register_receive_fn_t)(const char *app,
                                                     const char *version,
                                                     const char *release,
                                                     orcm_pnp_channel_t channel,
                                                     orcm_pnp_tag_t tag,
                                                     orcm_pnp_callback_fn_t cbfunc);

/* Cancel a receive - must provide the triplet and the channel (GROUP_OUTPUT or GROUP_INPUT)
 * and tag to get cancelled. A wildcard value for tag will cancel all receives on the
 * given channel. Likewise, a wildcard value for channel will cancel both output and
 * input receives
 */
typedef int (*orcm_pnp_module_cancel_recv_fn_t)(const char *app,
                                                const char *version,
                                                const char *release,
                                                orcm_pnp_channel_t channel,
                                                orcm_pnp_tag_t tag);


/*
 * Send an iovec stream or buffer from this process. If the process name is WILDCARD, or is NULL, then
 * the message will be multicast to the proper recipients. If the name is a specific one, then
 * the message will be directly sent to that process via an available point-to-point
 * protocol
 *
 * NOTE: only processes that have registered_input from this app/version/release
 * will actually receive the message. Thus, it should NOT be assumed that a process
 * on the given channel actually received all prior messages.
 */
typedef int (*orcm_pnp_module_output_fn_t)(orcm_pnp_channel_t channel,
                                           orte_process_name_t *recipient,
                                           orcm_pnp_tag_t tag,
                                           struct iovec *msg, int count,
                                           opal_buffer_t *buffer);

typedef int (*orcm_pnp_module_output_nb_fn_t)(orcm_pnp_channel_t channel,
                                              orte_process_name_t *recipient,
                                              orcm_pnp_tag_t tag,
                                              struct iovec *msg, int count,
                                              opal_buffer_t *buffer,
                                              orcm_pnp_callback_fn_t cbfunc,
                                              void *cbdata);

/* dynamically define a new tag */
typedef orcm_pnp_tag_t (*orcm_pnp_module_define_new_tag_fn_t)(void);

/* retrieve the triplet string id for this app */
typedef char* (*orcm_pnp_module_get_string_id_fn_t)(void);

/* component struct */
typedef struct {
    /** Base component description */
    mca_base_component_t pnpc_version;
    /** Base component data block */
    mca_base_component_data_t pnpc_data;
} orcm_pnp_base_component_2_0_0_t;
/** Convenience typedef */
typedef orcm_pnp_base_component_2_0_0_t orcm_pnp_base_component_t;

/* module struct */
typedef struct {
    orcm_pnp_module_init_fn_t                       init;
    orcm_pnp_module_announce_fn_t                   announce;
    orcm_pnp_module_open_channel_fn_t               open_channel;
    orcm_pnp_module_register_receive_fn_t           register_receive;
    orcm_pnp_module_cancel_recv_fn_t                cancel_receive;
    orcm_pnp_module_output_fn_t                     output;
    orcm_pnp_module_output_nb_fn_t                  output_nb;
    orcm_pnp_module_define_new_tag_fn_t             define_new_tag;
    orcm_pnp_module_get_string_id_fn_t              get_string_id;
    orcm_pnp_module_disable_comm_fn_t               disable_comm;
    orcm_pnp_module_finalize_fn_t                   finalize;
} orcm_pnp_base_module_t;

/** Interface for PNP communication */
ORCM_DECLSPEC extern orcm_pnp_base_module_t orcm_pnp;

/*
 * Macro for use in components that are of type coll
 */
#define ORCM_PNP_BASE_VERSION_1_0_0 \
  MCA_BASE_VERSION_2_0_0, \
  "orcm_pnp", 1, 0, 0

#endif /* ORCM_PNP_H */

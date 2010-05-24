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

#include "pnp_types.h"

/* module functions */
typedef int (*orcm_pnp_module_init_fn_t)(void);
typedef int (*orcm_pnp_module_finalize_fn_t)(void);

/*
 * Announce my existence, specifying my application's name, version
 * and release. All three fields MUST be provided or an error will
 * be returned. If non-NULL, the cbfunc will be called whenever a new
 * group announcement is received. The callback will return the
 * app/version/release triplet of the new group, along with its
 * pnp channel id.
 */
typedef int (*orcm_pnp_module_announce_fn_t)(char *app, char *version, char *release,
                                             orcm_pnp_announce_fn_t cbfunc);

/*
 * Open a channel to another app/version/release triplet. A callback will be
 * made whenever the first announcement is recvd from a process matching the specified
 * triplet (NULL => WILDCARD for that field, will recv callback for each unique
 * triplet that fits)
 */
typedef int (*orcm_pnp_module_open_channel_fn_t)(char *app, char *version, char *release,
                                                 orcm_pnp_open_channel_cbfunc_t cbfunc);

/*
 * Register to receive messages from the specified app/version/release.
 * The callback function MUST be specified. Providing a NULL for app, version,
 and/or release acts as a wildcard for those values.
 *
 * NOTE: because it would be impossible to unpack a message that was sent
 * as an array of iovecs as if it were a buffer, an application's output type
 * will be checked against only its corresponding registered input type. Thus,
 * a message sent as a buffer will only be delivered to a callback function
 * specified in a register_input_buffer call.
 */
typedef int (*orcm_pnp_module_register_input_fn_t)(char *app,
                                                   char *version,
                                                   char *release,
                                                   orcm_pnp_tag_t tag,
                                                   orcm_pnp_callback_fn_t cbfunc);

/*
 * Register to receive message buffers instead of iovec arrays
 */
typedef int (*orcm_pnp_module_register_input_buffer_fn_t)(char *app,
                                                          char *version,
                                                          char *release,
                                                          orcm_pnp_tag_t tag,
                                                          orcm_pnp_callback_buffer_fn_t cbfunc);

/* Deregister an input */
typedef int (*orcm_pnp_module_deregister_input_fn_t)(char *app,
                                                     char *version,
                                                     char *release,
                                                     orcm_pnp_tag_t tag);


/*
 * Send an iovec stream from this process. If the process name is WILDCARD, or is NULL, then
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
                                           struct iovec *msg, int count);

typedef int (*orcm_pnp_module_output_nb_fn_t)(orcm_pnp_channel_t channel,
                                              orte_process_name_t *recipient,
                                              orcm_pnp_tag_t tag,
                                              struct iovec *msg, int count,
                                              orcm_pnp_callback_fn_t cbfunc,
                                              void *cbdata);
/*
 * Send a buffer from this process, subject to same notes as above
 */
typedef int (*orcm_pnp_module_output_buffer_fn_t)(orcm_pnp_channel_t channel,
                                                  orte_process_name_t *recipient,
                                                  orcm_pnp_tag_t tag,
                                                  opal_buffer_t *buffer);

typedef int (*orcm_pnp_module_output_buffer_nb_fn_t)(orcm_pnp_channel_t channel,
                                                     orte_process_name_t *recipient,
                                                     orcm_pnp_tag_t tag,
                                                     opal_buffer_t *buffer,
                                                     orcm_pnp_callback_buffer_fn_t cbfunc,
                                                     void *cbdata);

/* dynamically define a new tag */
typedef orcm_pnp_tag_t (*orcm_pnp_module_define_new_tag_fn_t)(void);

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
    orcm_pnp_module_register_input_fn_t             register_input;
    orcm_pnp_module_register_input_buffer_fn_t      register_input_buffer;
    orcm_pnp_module_deregister_input_fn_t           deregister_input;
    orcm_pnp_module_output_fn_t                     output;
    orcm_pnp_module_output_nb_fn_t                  output_nb;
    orcm_pnp_module_output_buffer_fn_t              output_buffer;
    orcm_pnp_module_output_buffer_nb_fn_t           output_buffer_nb;
    orcm_pnp_module_define_new_tag_fn_t             define_new_tag;
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

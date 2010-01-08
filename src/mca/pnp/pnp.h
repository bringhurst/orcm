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

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#include <errno.h>

#include "opal/mca/mca.h"
#include "opal/dss/dss_types.h"

#include "orte/types.h"

#include "pnp_types.h"

/* callback function */
typedef void (*orcm_pnp_callback_fn_t)(int status,
                                       orte_process_name_t *sender,
                                       orcm_pnp_tag_t tag,
                                       struct iovec *msg,
                                       int count,
                                       void *cbdata);

typedef void (*orcm_pnp_callback_buffer_fn_t)(int status,
                                              orte_process_name_t *sender,
                                              orcm_pnp_tag_t tag,
                                              opal_buffer_t *buf,
                                              void *cbdata);

/* module functions */
typedef int (*orcm_pnp_module_init_fn_t)(void);
typedef int (*orcm_pnp_module_finalize_fn_t)(void);

/*
 * Announce my existence, specifying my application's name, version
 * and release. The application name MUST be provided. However, the
 * remaining info can be NULL if desired
 */
typedef int (*orcm_pnp_module_announce_fn_t)(char *app, char *version, char *release);

/*
 * Register to receive messages from the specified app/version/release.
 * The application name and callback function MUST be specified. Providing
 * a NULL for version and/or release acts as a wildcard for those values
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
 */
typedef int (*orcm_pnp_module_output_fn_t)(orte_process_name_t *recipient,
                                           orcm_pnp_tag_t tag,
                                           struct iovec *msg, int count);

typedef int (*orcm_pnp_module_output_nb_fn_t)(orte_process_name_t *recipient,
                                              orcm_pnp_tag_t tag,
                                              struct iovec *msg, int count,
                                              orcm_pnp_callback_fn_t cbfunc,
                                              void *cbdata);
/*
 * Send a buffer from this process
 */
typedef int (*orcm_pnp_module_output_buffer_fn_t)(orte_process_name_t *recipient,
                                                  orcm_pnp_tag_t tag,
                                                  opal_buffer_t *buffer);

typedef int (*orcm_pnp_module_output_buffer_nb_fn_t)(orte_process_name_t *recipient,
                                                     orcm_pnp_tag_t tag,
                                                     opal_buffer_t *buffer,
                                                     orcm_pnp_callback_buffer_fn_t cbfunc,
                                                     void *cbdata);

/* get the multicast group object for the specified app-triplet */
typedef orcm_pnp_group_t* (*orcm_pnp_module_get_group_fn_t)(char *app, char *version, char *release);

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
    orcm_pnp_module_register_input_fn_t             register_input;
    orcm_pnp_module_register_input_buffer_fn_t      register_input_buffer;
    orcm_pnp_module_deregister_input_fn_t           deregister_input;
    orcm_pnp_module_output_fn_t                     output;
    orcm_pnp_module_output_nb_fn_t                  output_nb;
    orcm_pnp_module_output_buffer_fn_t              output_buffer;
    orcm_pnp_module_output_buffer_nb_fn_t           output_buffer_nb;
    orcm_pnp_module_get_group_fn_t                  get_group;
    orcm_pnp_module_define_new_tag_fn_t             define_new_tag;
    orcm_pnp_module_finalize_fn_t                   finalize;
} orcm_pnp_base_module_t;

/** Interface for PNP communication */
ORCM_DECLSPEC extern orcm_pnp_base_module_t orcm_pnp;

/*
 * Macro for use in components that are of type coll
 */
#define ORCM_PNP_BASE_VERSION_2_0_0 \
  MCA_BASE_VERSION_2_0_0, \
  "pnp", 2, 0, 0

#endif /* ORCM_PNP_H */

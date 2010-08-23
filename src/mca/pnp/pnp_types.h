/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef ORCM_PNP_TYPES_H
#define ORCM_PNP_TYPES_H

#include "openrcm.h"

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#include "opal/dss/dss_types.h"
#include "opal/class/opal_list.h"
#include "opal/class/opal_pointer_array.h"
#include "opal/threads/condition.h"
#include "opal/threads/mutex.h"

#include "orte/types.h"
#include "orte/mca/rmcast/rmcast_types.h"

#include "runtime/orcm_globals.h"

BEGIN_C_DECLS

typedef int32_t orcm_pnp_tag_t;
#define ORCM_PNP_TAG_T  OPAL_INT32

/* inherited tag values */
enum {
    ORCM_PNP_TAG_WILDCARD       = ORTE_RMCAST_TAG_WILDCARD,
    ORCM_PNP_TAG_INVALID        = ORTE_RMCAST_TAG_INVALID,
    ORCM_PNP_TAG_BOOTSTRAP      = ORTE_RMCAST_TAG_BOOTSTRAP,
    ORCM_PNP_TAG_ANNOUNCE       = ORTE_RMCAST_TAG_ANNOUNCE,
    ORCM_PNP_TAG_OUTPUT         = ORTE_RMCAST_TAG_OUTPUT,
    ORCM_PNP_TAG_PS             = ORTE_RMCAST_TAG_PS,
    ORCM_PNP_TAG_MSG            = ORTE_RMCAST_TAG_MSG,
    ORCM_PNP_TAG_TOOL           = ORTE_RMCAST_TAG_TOOL,
    ORCM_PNP_TAG_IOF            = ORTE_RMCAST_TAG_IOF,
    ORCM_PNP_TAG_DATA           = ORTE_RMCAST_TAG_DATA,
    ORCM_PNP_TAG_CMD_ACK        = ORTE_RMCAST_TAG_CMD_ACK,
    ORCM_PNP_TAG_HEARTBEAT      = ORTE_RMCAST_TAG_HEARTBEAT,
    ORCM_PNP_TAG_COMMAND        = ORTE_RMCAST_TAG_COMMAND,
    ORCM_PNP_TAG_ERRMGR         = ORTE_RMCAST_TAG_ERRMGR
};

#define ORCM_PNP_TAG_DYNAMIC    100

/* inherited channels */
enum {
    ORCM_PNP_GROUP_INPUT_CHANNEL    = ORTE_RMCAST_GROUP_INPUT_CHANNEL,
    ORCM_PNP_GROUP_OUTPUT_CHANNEL   = ORTE_RMCAST_GROUP_OUTPUT_CHANNEL,
    ORCM_PNP_WILDCARD_CHANNEL       = ORTE_RMCAST_WILDCARD_CHANNEL,
    ORCM_PNP_INVALID_CHANNEL        = ORTE_RMCAST_INVALID_CHANNEL,
    ORCM_PNP_SYS_CHANNEL            = ORTE_RMCAST_SYS_CHANNEL,
    ORCM_PNP_APP_PUBLIC_CHANNEL     = ORTE_RMCAST_APP_PUBLIC_CHANNEL,
    ORCM_PNP_DATA_SERVER_CHANNEL    = ORTE_RMCAST_DATA_SERVER_CHANNEL,
    ORCM_PNP_ERROR_CHANNEL          = ORTE_RMCAST_ERROR_CHANNEL
};

#define ORCM_PNP_DYNAMIC_CHANNELS   ORTE_RMCAST_DYNAMIC_CHANNELS

/* callback functions */
typedef void (*orcm_pnp_announce_fn_t)(char *app, char *version, char *release,
                                       orte_process_name_t *name, char *node,
                                       char *rml_uri, uint32_t uid);

typedef void (*orcm_pnp_callback_fn_t)(int status,
                                       orte_process_name_t *sender,
                                       orcm_pnp_tag_t tag,
                                       struct iovec *msg,
                                       int count,
                                       opal_buffer_t *buf,
                                       void *cbdata);

END_C_DECLS

#endif /* ORCM_PNP_TYPES_H */

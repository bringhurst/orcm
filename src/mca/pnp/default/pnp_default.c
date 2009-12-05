/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config.h"
#include "include/constants.h"

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#include <errno.h>

#include "opal/dss/dss.h"
#include "opal/util/output.h"

#include "orte/mca/rmcast/rmcast.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rml/rml.h"
#include "orte/runtime/orte_globals.h"

#include "mca/leader/leader.h"

#include "mca/pnp/pnp.h"
#include "mca/pnp/base/private.h"
#include "mca/pnp/default/pnp_default.h"

/* API functions */

static int default_init(void);
static int announce(char *app, char *version, char *release);
static int register_input(char *app,
                          char *version,
                          char *release,
                          orcm_pnp_tag_t tag,
                          orcm_pnp_callback_fn_t cbfunc);
static int register_input_buffer(char *app,
                                 char *version,
                                 char *release,
                                 orcm_pnp_tag_t tag,
                                 orcm_pnp_callback_buffer_fn_t cbfunc);
static int deregister_input(char *app,
                            char *version,
                            char *release,
                            orcm_pnp_tag_t tag);
static int default_output(orte_process_name_t *recipient,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg, int count);
static int default_output_nb(orte_process_name_t *recipient,
                             orcm_pnp_tag_t tag,
                             struct iovec *msg, int count,
                             orcm_pnp_callback_fn_t cbfunc,
                             void *cbdata);
static int default_output_buffer(orte_process_name_t *recipient,
                                 orcm_pnp_tag_t tag,
                                 opal_buffer_t *buffer);
static int default_output_buffer_nb(orte_process_name_t *recipient,
                                    orcm_pnp_tag_t tag,
                                    opal_buffer_t *buffer,
                                    orcm_pnp_callback_buffer_fn_t cbfunc,
                                    void *cbdata);
static orcm_pnp_tag_t define_new_tag(void);
static int default_finalize(void);

/* The module struct */

orcm_pnp_base_module_t orcm_pnp_default_module = {
    default_init,
    announce,
    register_input,
    register_input_buffer,
    deregister_input,
    default_output,
    default_output_nb,
    default_output_buffer,
    default_output_buffer_nb,
    default_finalize
};

/* Local functions */
static void recv_announcements(int status,
                               orte_rmcast_channel_t channel,
                               orte_rmcast_tag_t tag,
                               orte_process_name_t *sender,
                               orte_rmcast_seq_t seq_num,
                               opal_buffer_t *buf, void *cbdata);
static void recv_inputs(int status,
                        orte_rmcast_channel_t channel,
                        orte_rmcast_tag_t tag,
                        orte_process_name_t *sender,
                        orte_rmcast_seq_t seq_num,
                        struct iovec *msg, int count, void *cbdata);
static void recv_input_buffers(int status,
                               orte_rmcast_channel_t channel,
                               orte_rmcast_tag_t tag,
                               orte_process_name_t *sender,
                               orte_rmcast_seq_t seq_num,
                               opal_buffer_t *buf, void *cbdata);
static int pack_announcement(opal_buffer_t *buf);

static void rmcast_callback_buffer(int status,
                                   orte_rmcast_channel_t channel,
                                   orte_rmcast_tag_t tag,
                                   orte_process_name_t *sender,
                                   orte_rmcast_seq_t seq_num,
                                   opal_buffer_t *buf, void* cbdata);

static void rmcast_callback(int status,
                            orte_rmcast_channel_t channel,
                            orte_rmcast_tag_t tag,
                            orte_process_name_t *sender,
                            orte_rmcast_seq_t seq_num,
                            struct iovec *msg, int count, void* cbdata);

static void rml_callback(int status,
                         struct orte_process_name_t* peer,
                         struct iovec* msg,
                         int count,
                         orte_rml_tag_t tag,
                         void* cbdata);
                         
static void rml_callback_buffer(int status,
                                struct orte_process_name_t* peer,
                                struct opal_buffer_t* buffer,
                                orte_rml_tag_t tag,
                                void* cbdata);

/* Local variables */
static opal_list_t groups;
static int32_t packet_number = 0;
static bool recv_on = false;
static char *my_app = NULL;
static char *my_version = NULL;
static char *my_release = NULL;

static int default_init(void)
{
    int ret;
    
    /* init the list of known application groups */
    OBJ_CONSTRUCT(&groups, opal_list_t);
    
    /* setup a recv to catch any announcements */
    if (!recv_on) {
        if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(ORTE_RMCAST_APP_PUBLIC_CHANNEL,
                                                              ORTE_RMCAST_TAG_ANNOUNCE,
                                                              ORTE_RMCAST_PERSISTENT,
                                                              recv_announcements, NULL))) {
            ORTE_ERROR_LOG(ret);
            return ret;
        }
        recv_on = true;
    }
    
    return ORCM_SUCCESS;
}

static int announce(char *app, char *version, char *release)
{
    int ret;
    opal_buffer_t buf;
    
    /* bozo check */
    if (NULL == app || NULL == version || NULL == release) {
        ORTE_ERROR_LOG(ORCM_ERR_BAD_PARAM);
        return ORCM_ERR_BAD_PARAM;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:announce app %s version %s release %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         app, version, release));

    /* retain a local record of my info */
    if (NULL == my_app) {
        my_app = strdup(app);
    }
    if (NULL == my_version) {
        my_version = strdup(version);
    }
    if (NULL == my_release) {
        my_release = strdup(release);
    }
    
    /* assemble the announcement message */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    
    /* pack the common elements */
    if (ORCM_SUCCESS != (ret = pack_announcement(&buf))) {
        ORTE_ERROR_LOG(ret);
        OBJ_DESTRUCT(&buf);
        return ret;
    }
    /* this is an original announcement, so we are responding to nobody */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, ORTE_NAME_INVALID, 1, ORTE_NAME))) {
        ORTE_ERROR_LOG(ret);
        OBJ_DESTRUCT(&buf);
        return ret;
    }
    
    /* send it */
    if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer(ORTE_RMCAST_APP_PUBLIC_CHANNEL,
                                                      ORTE_RMCAST_TAG_ANNOUNCE,
                                                      &buf))) {
        ORTE_ERROR_LOG(ret);
    }
    
    /* cleanup */
    OBJ_DESTRUCT(&buf);
    return ret;
}

static int register_input(char *app,
                          char *version,
                          char *release,
                          orcm_pnp_tag_t tag,
                          orcm_pnp_callback_fn_t cbfunc)
{
    opal_list_item_t *item;
    orcm_pnp_group_t *group;
    bool found;
    size_t sz;
    orte_rmcast_channel_t channel;
    orcm_pnp_pending_request_t *request;
    int ret;
    
    /* bozo check */
    if (NULL == app) {
        ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
        return ORTE_ERR_BAD_PARAM;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:register_input app %s version %s release %s tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         app, (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release, tag));
    
    found = false;
    for (item = opal_list_get_first(&groups);
         item != opal_list_get_end(&groups);
         item = opal_list_get_next(item)) {
        group = (orcm_pnp_group_t*)item;
        
        if (0 == strcasecmp(group->app, app) &&
            (NULL == version || 0 == strcasecmp(group->version, version)) &&
            (NULL == release || 0 == strcasecmp(group->release, release))) {
            /* record the request */
            request = OBJ_NEW(orcm_pnp_pending_request_t);
            request->tag = tag;
            request->cbfunc = cbfunc;
            opal_list_append(&group->requests, &request->super);
            
            /* ensure we are listening for this tag on all known channels */
            for (sz=0; sz < opal_value_array_get_size(&group->channels); sz++) {
                channel = OPAL_VALUE_ARRAY_GET_ITEM(&group->channels, orte_rmcast_channel_t, sz);
                OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                     "%s pnp:default:register_input opening channel %d",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), channel));
                /* open a channel to this group - will just return if
                 * the channel already exists
                 */
                if (ORCM_SUCCESS != (ret = orte_rmcast.open_channel(&channel,
                                                                   group->app,
                                                                   NULL, -1, NULL,
                                                                   ORTE_RMCAST_RECV))) {
                    ORTE_ERROR_LOG(ret);
                    return;
                }
                /* setup to listen to it - will just return if we already are */
                if (ORTE_SUCCESS != (ret = orte_rmcast.recv_nb(channel, tag,
                                                               ORTE_RMCAST_PERSISTENT,
                                                               recv_inputs, cbfunc))) {
                    ORTE_ERROR_LOG(ret);
                    return ret;
                }
            }
            found = true;
        }
    }
    
    /* if we didn't find at least one, then create the group
     * and indicate that we want to listen to it if/when
     * it is announced
     */
    if (!found) {
        group = OBJ_NEW(orcm_pnp_group_t);
        group->app = strdup(app);
        if (NULL != version) {
            group->version = strdup(version);
        }
        if (NULL != release) {
            group->release = strdup(release);
        }
        /* record the request */
        request = OBJ_NEW(orcm_pnp_pending_request_t);
        request->tag = tag;
        request->cbfunc = cbfunc;
        opal_list_append(&group->requests, &request->super);
        /* add to the list of groups */
        opal_list_append(&groups, &group->super);
    }
    
    return ORCM_SUCCESS;
}

static int register_input_buffer(char *app,
                                 char *version,
                                 char *release,
                                 orcm_pnp_tag_t tag,
                                 orcm_pnp_callback_buffer_fn_t cbfunc)
{
    opal_list_item_t *item;
    orcm_pnp_group_t *group;
    bool found;
    size_t sz;
    orte_rmcast_channel_t channel;
    orcm_pnp_pending_request_t *request;
    int ret;
    
    /* bozo check */
    if (NULL == app) {
        ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
        return ORTE_ERR_BAD_PARAM;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:register_input_buffer app %s version %s release %s tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         app, (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release, tag));
    
    found = false;
    for (item = opal_list_get_first(&groups);
         item != opal_list_get_end(&groups);
         item = opal_list_get_next(item)) {
        group = (orcm_pnp_group_t*)item;
        
        if (0 == strcasecmp(group->app, app) &&
            (NULL == version || 0 == strcasecmp(group->version, version)) &&
            (NULL == release || 0 == strcasecmp(group->release, release))) {
            /* ensure we are listening on all known channels */
            for (sz=0; sz < opal_value_array_get_size(&group->channels); sz++) {
                channel = OPAL_VALUE_ARRAY_GET_ITEM(&group->channels, orte_rmcast_channel_t, sz);
                OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                     "%s pnp:default:register_input_buffer opening channel %d",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), channel));
                /* open a channel to this group - will just return if
                 * the channel already exists
                 */
                if (ORCM_SUCCESS != (ret = orte_rmcast.open_channel(&channel,
                                                                   group->app,
                                                                   NULL, -1, NULL,
                                                                   ORTE_RMCAST_RECV))) {
                    ORTE_ERROR_LOG(ret);
                    return;
                }
                /* setup to listen to it - will just return if we already are */
                if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(channel, tag,
                                                                      ORTE_RMCAST_PERSISTENT,
                                                                      recv_input_buffers, cbfunc))) {
                    ORTE_ERROR_LOG(ret);
                    return ret;
                }
            }
            found = true;
        }
    }
    
    /* if we didn't find at least one, then create the group
     * and indicate that we want to listen to it if/when
     * it is announced
     */
    if (!found) {
        group = OBJ_NEW(orcm_pnp_group_t);
        group->app = strdup(app);
        if (NULL != version) {
            group->version = strdup(version);
        }
        if (NULL != release) {
            group->release = strdup(release);
        }
        /* record the request */
        request = OBJ_NEW(orcm_pnp_pending_request_t);
        request->tag = tag;
        request->cbfunc_buf = cbfunc;
        opal_list_append(&group->requests, &request->super);
        /* add to the list of groups */
        opal_list_append(&groups, &group->super);
    }
    
    return ORCM_SUCCESS;
}

static int deregister_input(char *app,
                            char *version,
                            char *release,
                            orcm_pnp_tag_t tag)
{
    opal_list_item_t *item;
    orcm_pnp_group_t *group;
    orte_rmcast_channel_t channel;
    size_t sz;
    int ret;
    
    /* bozo check */
    if (NULL == app) {
        ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
        return ORTE_ERR_BAD_PARAM;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:deregister_input app %s version %s release %s tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         app, (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release, tag));
    
    for (item = opal_list_get_first(&groups);
         item != opal_list_get_end(&groups);
         item = opal_list_get_next(item)) {
        group = (orcm_pnp_group_t*)item;
        
        if (0 == strcasecmp(group->app, app) &&
            (NULL == version || 0 == strcasecmp(group->version, version)) &&
            (NULL == release || 0 == strcasecmp(group->release, release))) {
            for (sz=0; sz < opal_value_array_get_size(&group->channels); sz++) {
                channel = OPAL_VALUE_ARRAY_GET_ITEM(&group->channels, orte_rmcast_channel_t, sz);
                OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                     "%s pnp:default:deregister_input closing channel %d",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), channel));
                orte_rmcast.cancel_recv(channel, tag);
            }
        }
    }
    
    return ORCM_SUCCESS;
}

static int default_output(orte_process_name_t *recipient,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg, int count)
{
    int ret;
    
    /* if this is intended for everyone who might be listening to my output,
     * multicast it
     */
    if (NULL == recipient ||
        (ORTE_JOBID_WILDCARD == recipient->jobid &&
         ORTE_VPID_WILDCARD == recipient->vpid)) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:sending multicast of %d iovecs to tag %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), count, tag));
        
        /* send the iovecs to my group output channel */
        if (ORCM_SUCCESS != (ret = orte_rmcast.send(ORTE_RMCAST_GROUP_OUTPUT_CHANNEL,
                                                   tag, msg, count))) {
            ORTE_ERROR_LOG(ret);
        }
        return ret;
    }
    
    /* if only one name field is WILDCARD, I don't know how to send
     * it - at least, not right now
     */
    if (ORTE_JOBID_WILDCARD == recipient->jobid ||
        ORTE_VPID_WILDCARD == recipient->vpid) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_IMPLEMENTED);
        return ORTE_ERR_NOT_IMPLEMENTED;
    }
    
    /* intended for a specific recipient, send it over p2p */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending p2p message of %d iovecs to tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), count, tag));
    if (ORCM_SUCCESS != (ret = orte_rml.send(recipient, msg, count, tag, 0))) {
        ORTE_ERROR_LOG(ret);
    }
    return ret;
}

static int default_output_nb(orte_process_name_t *recipient,
                             orcm_pnp_tag_t tag,
                             struct iovec *msg, int count,
                             orcm_pnp_callback_fn_t cbfunc,
                             void *cbdata)
{
    int ret;
    orcm_pnp_send_t *send;
    
    send = OBJ_NEW(orcm_pnp_send_t);
    send->cbfunc = cbfunc;
    send->cbdata = cbdata;
    
    /* if this is intended for everyone who might be listening to my output,
     * multicast it
     */
    if (NULL == recipient ||
        (ORTE_JOBID_WILDCARD == recipient->jobid &&
         ORTE_VPID_WILDCARD == recipient->vpid)) {
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s pnp:default:sending multicast of %d iovecs to tag %d",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), count, tag));
            
            /* send the iovecs to my group output channel */
            if (ORCM_SUCCESS != (ret = orte_rmcast.send_nb(ORTE_RMCAST_GROUP_OUTPUT_CHANNEL,
                                                         tag, msg, count, rmcast_callback, send))) {
                ORTE_ERROR_LOG(ret);
            }
            return ret;
        }
    
    /* if only one name field is WILDCARD, I don't know how to send
     * it - at least, not right now
     */
    if (ORTE_JOBID_WILDCARD == recipient->jobid ||
        ORTE_VPID_WILDCARD == recipient->vpid) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_IMPLEMENTED);
        return ORTE_ERR_NOT_IMPLEMENTED;
    }
    
    /* intended for a specific recipient, send it over p2p */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending p2p message of %d iovecs to tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), count, tag));
    if (ORCM_SUCCESS != (ret = orte_rml.send_nb(recipient, msg, count, tag, 0, rml_callback, send))) {
        ORTE_ERROR_LOG(ret);
    }
    return ret;
}

static int default_output_buffer(orte_process_name_t *recipient,
                                 orcm_pnp_tag_t tag,
                                 opal_buffer_t *buffer)
{
    int ret;
    
    /* if this is intended for everyone who might be listening to my output,
     * multicast it
     */
    if (NULL == recipient ||
        (ORTE_JOBID_WILDCARD == recipient->jobid &&
         ORTE_VPID_WILDCARD == recipient->vpid)) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:sending multicast buffer of %d bytes to tag %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)buffer->bytes_used, tag));
        
        /* send the buffer to my group output channel */
        if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer(ORTE_RMCAST_GROUP_OUTPUT_CHANNEL,
                                                          tag, buffer))) {
            ORTE_ERROR_LOG(ret);
        }
        return ret;
    }
    
    /* if only one name field is WILDCARD, I don't know how to send
     * it - at least, not right now
     */
    if (ORTE_JOBID_WILDCARD == recipient->jobid ||
        ORTE_VPID_WILDCARD == recipient->vpid) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_IMPLEMENTED);
        return ORTE_ERR_NOT_IMPLEMENTED;
    }
    
    /* intended for a specific recipient, send it over p2p */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending p2p buffer of %d bytes to tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)buffer->bytes_used, tag));
    if (ORCM_SUCCESS != (ret = orte_rml.send_buffer(recipient, buffer, tag, 0))) {
        ORTE_ERROR_LOG(ret);
    }
    return ret;
}

static int default_output_buffer_nb(orte_process_name_t *recipient,
                                    orcm_pnp_tag_t tag,
                                    opal_buffer_t *buffer,
                                    orcm_pnp_callback_buffer_fn_t cbfunc,
                                    void *cbdata)
{
    int ret;
    orcm_pnp_send_t *send;
    
    send = OBJ_NEW(orcm_pnp_send_t);
    send->cbfunc_buf = cbfunc;
    send->cbdata = cbdata;
    
    /* if this is intended for everyone who might be listening to my output,
     * multicast it
     */
    if (NULL == recipient ||
        (ORTE_JOBID_WILDCARD == recipient->jobid &&
         ORTE_VPID_WILDCARD == recipient->vpid)) {
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s pnp:default:sending multicast buffer of %d bytes to tag %d",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)buffer->bytes_used, tag));
            
            /* send the buffer to my group output channel */
            if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer_nb(ORTE_RMCAST_GROUP_OUTPUT_CHANNEL,
                                                                  tag, buffer,
                                                                  rmcast_callback_buffer, send))) {
                ORTE_ERROR_LOG(ret);
            }
            return ret;
        }
    
    /* if only one name field is WILDCARD, I don't know how to send
     * it - at least, not right now
     */
    if (ORTE_JOBID_WILDCARD == recipient->jobid ||
        ORTE_VPID_WILDCARD == recipient->vpid) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_IMPLEMENTED);
        return ORTE_ERR_NOT_IMPLEMENTED;
    }
    
    /* intended for a specific recipient, send it over p2p */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending p2p buffer of %d bytes to tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)buffer->bytes_used, tag));
    if (ORCM_SUCCESS != (ret = orte_rml.send_buffer_nb(recipient, buffer, tag, 0,
                                                       rml_callback_buffer, send))) {
        ORTE_ERROR_LOG(ret);
    }
    return ret;
}

static orcm_pnp_tag_t define_new_tag(void)
{
    return ORCM_PNP_TAG_INVALID;
}

static int default_finalize(void)
{
    opal_list_item_t *item;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default: finalizing",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* cancel the recv, if active */
    if (recv_on) {
        orte_rmcast.cancel_recv(ORTE_RMCAST_APP_PUBLIC_CHANNEL, ORTE_RMCAST_TAG_ANNOUNCE);
        recv_on = false;
    }
    
    /* release the list of known application groups */
    while (NULL != (item = opal_list_remove_first(&groups))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&groups);
    
    return ORCM_SUCCESS;
}


/****    LOCAL  FUNCTIONS    ****/
static void recv_announcements(int status,
                               orte_rmcast_channel_t channel,
                               orte_rmcast_tag_t tag,
                               orte_process_name_t *sender,
                               orte_rmcast_seq_t seq_num,
                               opal_buffer_t *buf, void *cbdata)
{
    opal_list_item_t *item, *itm2;
    orcm_pnp_group_t *group;
    orcm_pnp_source_t *source;
    char *app, *version, *release;
    orte_process_name_t originator;
    size_t sz;
    bool found;
    opal_buffer_t ann;
    orte_rmcast_channel_t output;
    int rc, n;
    orcm_pnp_pending_request_t *request;
    
    /* unpack the app's name */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &app, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* get its version */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &version, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
        
    /* get its release */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &release, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* get its output channel */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &output, &n, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement from group app %s version %s release %s channel %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         app, version, release, output));

    /* do we already know this application group? */
    for (item = opal_list_get_first(&groups);
         item != opal_list_get_end(&groups);
         item = opal_list_get_next(item)) {
        group = (orcm_pnp_group_t*)item;
        
        if (0 == strcasecmp(group->app, app) &&
            0 == strcasecmp(group->version, version) &&
            0 == strcasecmp(group->release, release)) {
            /* yep - know this one */
            free(app);
            free(version);
            free(release);
            /* record the channel it is on, if it is a new one */
            found = false;
            for (sz=0; sz < opal_value_array_get_size(&group->channels); sz++) {
                if (output == OPAL_VALUE_ARRAY_GET_ITEM(&group->channels, orte_rmcast_channel_t, sz)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                     "%s pnp:default:recvd_ann adding new channel %d",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), output));
                opal_value_array_append_item(&group->channels, &output);
            }
            /* do we want to listen to this group? */
            if (0 == opal_list_get_size(&group->requests)) {
                /* nope - move along */
                goto senders;
            }
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s pnp:default:recvd_ann opening channel %d",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), output));
            /* open a channel to this group - will just return if
             * the channel already exists
             */
            if (ORCM_SUCCESS != (rc = orte_rmcast.open_channel(&output,
                                                              group->app, NULL, -1, NULL,
                                                              ORTE_RMCAST_RECV))) {
                ORTE_ERROR_LOG(rc);
                return;
            }
            /* cycle through all the requests and process them */
            for (itm2 = opal_list_get_first(&group->requests);
                 itm2 != opal_list_get_end(&group->requests);
                 itm2 = opal_list_get_next(itm2)) {
                request = (orcm_pnp_pending_request_t*)itm2;
 
                if (NULL != request->cbfunc) {
                    /* setup to listen to it - will just return if we already are */
                    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                         "%s pnp:default:defining recv_nb on channel %d tag %d",
                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), output, request->tag));
                    
                    if (ORTE_SUCCESS != (rc = orte_rmcast.recv_nb(output,
                                                                  ORTE_RMCAST_TAG_WILDCARD,
                                                                  ORTE_RMCAST_PERSISTENT,
                                                                  recv_inputs,
                                                                  request->cbfunc))) {
                        ORTE_ERROR_LOG(rc);
                        return;
                    }
                }
                if (NULL != request->cbfunc_buf) {
                    /* setup to listen to it - will just return if we already are */
                    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                         "%s pnp:default:defining recv_buffer_nb on channel %d tag %d",
                                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), output, request->tag));
                    
                    if (ORTE_SUCCESS != (rc = orte_rmcast.recv_buffer_nb(output,
                                                                         ORTE_RMCAST_TAG_WILDCARD,
                                                                         ORTE_RMCAST_PERSISTENT,
                                                                         recv_input_buffers,
                                                                         request->cbfunc_buf))) {
                        ORTE_ERROR_LOG(rc);
                        return;
                    }
                }
            }
        }
        goto senders;
    }
    
    /* if we get here, then this is a new application
     * group - add it to our list
     */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received_announcement has new group",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    group = OBJ_NEW(orcm_pnp_group_t);
    group->app = app;
    group->version = version;
    group->release = release;
    opal_value_array_append_item(&group->channels, &output);
    opal_list_append(&groups, &group->super);
    
senders:    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement from source %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));
    
    /* do we already know this source? */
    for (item = opal_list_get_first(&group->members);
         item != opal_list_get_end(&group->members);
         item = opal_list_get_next(item)) {
        source = (orcm_pnp_source_t*)item;
        
        if (sender->jobid == source->name.jobid &&
            sender->vpid == source->name.vpid) {
            goto response;
         }
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received adding source %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));
    
    /* if we get here, then this is a new source - add
     * it to our list
     */
    source = OBJ_NEW(orcm_pnp_source_t);
    source->name.jobid = sender->jobid;
    source->name.vpid = sender->vpid;
    opal_list_append(&group->members, &source->super);
    
response:
    /* who are they responding to? */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &originator, &n, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement from originator %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(&originator)));
    
    /* if they were responding to an announcement by someone,
     * then don't respond or else we'll go into an infinite
     * loop of announcements
     */
    if (originator.jobid != ORTE_JOBID_INVALID &&
        originator.vpid != ORTE_VPID_INVALID) {
        /* nothing more to do */
        return;
    }
    
    /* if we get here, then this is an original announcement,
     * so we need to let them know we are here
     */
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement sending response",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* assemble the announcement message */
    OBJ_CONSTRUCT(&ann, opal_buffer_t);
    
    /* pack the common elements */
    if (ORCM_SUCCESS != (rc = pack_announcement(&ann))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&ann);
        return;
    }
    /* tell everyone we are responding to an announcement
     * so they don't respond back
     */
    if (ORCM_SUCCESS != (rc = opal_dss.pack(&ann, sender, 1, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&ann);
        return;
    }
    
    /* send it */
    if (ORCM_SUCCESS != (rc = orte_rmcast.send_buffer(ORTE_RMCAST_APP_PUBLIC_CHANNEL,
                                                      ORTE_RMCAST_TAG_ANNOUNCE,
                                                      &ann))) {
        ORTE_ERROR_LOG(rc);
    }
    
    /* cleanup */
    OBJ_DESTRUCT(&ann);
    return;
}

static void recv_inputs(int status,
                        orte_rmcast_channel_t channel,
                        orte_rmcast_tag_t tag,
                        orte_process_name_t *sender,
                        orte_rmcast_seq_t seq_num,
                        struct iovec *msg, int count, void *cbdata)
{
    opal_list_item_t *item;
    orcm_pnp_group_t *group, *grp;
    orcm_pnp_source_t *source, *src;
    bool found;
    size_t sz;
    int rc;
    orcm_pnp_callback_fn_t cbfunc = (orcm_pnp_callback_fn_t)cbdata;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received input on channel %d from sender %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), channel,
                         ORTE_NAME_PRINT(sender)));
    
    /* locate this sender on our list */
    found = false;
    grp = NULL;
    for (item = opal_list_get_first(&groups);
         item != opal_list_get_end(&groups) && !found;
         item = opal_list_get_next(item)) {
        group = (orcm_pnp_group_t*)item;
        
        for (sz=0; sz < opal_value_array_get_size(&group->channels); sz++) {
            if (channel == OPAL_VALUE_ARRAY_GET_ITEM(&group->channels, orte_rmcast_channel_t, sz)) {
                grp = group;
                found = true;
                break;
            }
        }
    }
    if (NULL == grp) {
        /* we don't know this channel yet - just ignore it */
        return;
    }
    
    src = NULL;
    for (item = opal_list_get_first(&grp->members);
         item != opal_list_get_end(&grp->members);
         item = opal_list_get_next(item)) {
        source = (orcm_pnp_source_t*)item;
        
        if (sender->jobid == source->name.jobid &&
            sender->vpid == source->name.vpid) {
            src = source;
            break;
        }
    }
    if (NULL == src) {
        /* don't know this sender - ignore it */
        return;
    }
    
    /* if we haven't set a leader for this group, do so now */
    if (NULL == grp->leader) {
        grp->leader = src;
    }
    
    /* if the message is out of sequence... */
    if (!orcm_pnp_base_valid_sequence_number(src, seq_num)) {
        if (src == grp->leader) {
            /* ouch - we need a new leader */
            if (ORCM_SUCCESS != (rc = orcm_leader.set_leader(grp, NULL))) {
                ORTE_ERROR_LOG(rc);
            }
        } 
        /* remove this source from the list of members, thus
         * declaring it "invalid" from here forward
         */
        opal_list_remove_item(&grp->members, &src->super);
        OBJ_RELEASE(src);
        return;
    }
    /* update the msg number */
    src->last_msg_num = seq_num;
    
    /* if this data came from the leader, just use it */
    if (src == grp->leader) {
        cbfunc(ORCM_SUCCESS, sender, tag, msg, count, NULL);
    } else {
        /* check if we need a new leader */
        if (orcm_leader.has_leader_failed(grp)) {
            /* leader has failed - get new one */
            if (ORCM_SUCCESS != (rc = orcm_leader.set_leader(grp, NULL))) {
                ORTE_ERROR_LOG(rc);
            }
            /* if the sender is the new leader, deliver message */
            if (src == grp->leader) {
                cbfunc(ORCM_SUCCESS, sender, tag, msg, count, NULL);
            }
        }
    }
}

static void recv_input_buffers(int status,
                               orte_rmcast_channel_t channel,
                               orte_rmcast_tag_t tag,
                               orte_process_name_t *sender,
                               orte_rmcast_seq_t seq_num,
                               opal_buffer_t *buf, void *cbdata)
{
    opal_list_item_t *item;
    orcm_pnp_group_t *group, *grp;
    orcm_pnp_source_t *source, *src;
    bool found;
    size_t sz;
    int rc;
    orcm_pnp_callback_buffer_fn_t cbfunc = (orcm_pnp_callback_buffer_fn_t)cbdata;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received input buffer on channel %d from sender %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), channel,
                         ORTE_NAME_PRINT(sender)));
    
    /* locate this sender on our list */
    found = false;
    grp = NULL;
    for (item = opal_list_get_first(&groups);
         item != opal_list_get_end(&groups) && !found;
         item = opal_list_get_next(item)) {
        group = (orcm_pnp_group_t*)item;
        
        for (sz=0; sz < opal_value_array_get_size(&group->channels); sz++) {
            if (channel == OPAL_VALUE_ARRAY_GET_ITEM(&group->channels, orte_rmcast_channel_t, sz)) {
                grp = group;
                found = true;
                break;
            }
        }
    }
    if (NULL == grp) {
        /* we don't know this channel yet - just ignore it */
        return;
    }
    
    src = NULL;
    for (item = opal_list_get_first(&grp->members);
         item != opal_list_get_end(&grp->members);
         item = opal_list_get_next(item)) {
        source = (orcm_pnp_source_t*)item;
        
        if (sender->jobid == source->name.jobid &&
            sender->vpid == source->name.vpid) {
            src = source;
            break;
        }
    }
    if (NULL == src) {
        /* don't know this sender - ignore it */
        return;
    }
    
    /* if we haven't set a leader for this group, do so now */
    if (NULL == grp->leader) {
        grp->leader = src;
    }
    
    /* if the message is out of sequence... */
    if (!orcm_pnp_base_valid_sequence_number(src, seq_num)) {
        if (src == grp->leader) {
            /* ouch - we need a new leader */
            if (ORCM_SUCCESS != (rc = orcm_leader.set_leader(grp, NULL))) {
                ORTE_ERROR_LOG(rc);
            }
        } 
        /* remove this source from the list of members, thus
         * declaring it "invalid" from here forward
         */
        opal_list_remove_item(&grp->members, &src->super);
        OBJ_RELEASE(src);
        return;
    }
    /* update the msg number */
    src->last_msg_num = seq_num;
    
    /* if this data came from the leader, just use it */
    if (src == grp->leader) {
        cbfunc(ORCM_SUCCESS, sender, tag, buf, NULL);
    } else {
        /* check if we need a new leader */
        if (orcm_leader.has_leader_failed(grp)) {
            /* leader has failed - get new one */
            if (ORCM_SUCCESS != (rc = orcm_leader.set_leader(grp, NULL))) {
                ORTE_ERROR_LOG(rc);
            }
            /* if the sender is the new leader, deliver message */
            if (src == grp->leader) {
                cbfunc(ORCM_SUCCESS, sender, tag, buf, NULL);
            }
        }
    }
}

/* pack the common elements of an announcement message */
static int pack_announcement(opal_buffer_t *buf)
{
    int ret;
    orte_rmcast_channel_t my_channel;
    
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_app, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_version, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_release, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    /* get and pack my output channel */
    my_channel = orte_rmcast.query_channel();
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_channel, 1, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    
    return ORCM_SUCCESS;
}

/* ORTE callback functions so we can map them to our own */
static void rmcast_callback_buffer(int status,
                                   orte_rmcast_channel_t channel,
                                   orte_rmcast_tag_t tag,
                                   orte_process_name_t *sender,
                                   orte_rmcast_seq_t seq_num,
                                   opal_buffer_t *buf, void* cbdata)
{
    orcm_pnp_send_t *send = (orcm_pnp_send_t*)cbdata;
    
    if (NULL != send->cbfunc_buf) {
        send->cbfunc_buf(status, ORTE_PROC_MY_NAME, tag, buf, send->cbdata);
    }
    OBJ_RELEASE(send);
}

static void rmcast_callback(int status,
                            orte_rmcast_channel_t channel,
                            orte_rmcast_tag_t tag,
                            orte_process_name_t *sender,
                            orte_rmcast_seq_t seq_num,
                            struct iovec *msg, int count, void* cbdata)
{
    orcm_pnp_send_t *send = (orcm_pnp_send_t*)cbdata;
    
    if (NULL != send->cbfunc) {
        send->cbfunc(status, ORTE_PROC_MY_NAME, tag, msg, count, send->cbdata);
    }
    OBJ_RELEASE(send);
}

static void rml_callback(int status,
                         orte_process_name_t* sender,
                         struct iovec* msg,
                         int count,
                         orte_rml_tag_t tag,
                         void* cbdata)
{
    orcm_pnp_send_t *send = (orcm_pnp_send_t*)cbdata;
    
    if (NULL != send->cbfunc) {
        send->cbfunc(status, ORTE_PROC_MY_NAME, tag, msg, count, send->cbdata);
    }
    OBJ_RELEASE(send);
}

static void rml_callback_buffer(int status,
                                orte_process_name_t* sender,
                                opal_buffer_t* buffer,
                                orte_rml_tag_t tag,
                                void* cbdata)
{
    orcm_pnp_send_t *send = (orcm_pnp_send_t*)cbdata;
    
    if (NULL != send->cbfunc_buf) {
        send->cbfunc_buf(status, ORTE_PROC_MY_NAME, tag, buffer, send->cbdata);
    }
    OBJ_RELEASE(send);
}

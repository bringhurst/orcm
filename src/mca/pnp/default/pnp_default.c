/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config_private.h"
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
static orcm_pnp_group_t* get_group(char *app, char *version, char *release);
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
    get_group,
    define_new_tag,
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

static void process_direct_msgs(int fd, short event, void *cbdata);

static void recv_direct_msgs(int status, orte_process_name_t* sender,
                             opal_buffer_t* buffer, orte_rml_tag_t tag,
                             void* cbdata);

/* Local variables */
static opal_list_t groups;
static int32_t packet_number = 0;
static bool recv_on = false;
static char *my_app = NULL;
static char *my_version = NULL;
static char *my_release = NULL;
static opal_mutex_t lock;
static opal_list_t recvs;
static opal_event_t ready;
static int ready_fd[2];
static bool processing;

static int default_init(void)
{
    int ret;
    
    /* init the list of known application groups */
    OBJ_CONSTRUCT(&groups, opal_list_t);
    
    /* setup the threading support */
    processing = false;
    OBJ_CONSTRUCT(&lock, opal_mutex_t);
    OBJ_CONSTRUCT(&recvs, opal_list_t);
    pipe(ready_fd);
    opal_event_set(&ready, ready_fd[0], OPAL_EV_READ, process_direct_msgs, NULL);
    opal_event_add(&ready, 0);

    /* setup a recv to catch any announcements */
    if (!recv_on) {
        if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(ORTE_RMCAST_APP_PUBLIC_CHANNEL,
                                                              ORTE_RMCAST_TAG_ANNOUNCE,
                                                              ORTE_RMCAST_PERSISTENT,
                                                              recv_announcements, NULL))) {
            ORTE_ERROR_LOG(ret);
            return ret;
        }
        /* setup an RML recv to catch any direct messages */
        if (ORTE_SUCCESS != (ret = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD,
                                                           ORTE_RML_TAG_MULTICAST_DIRECT,
                                                           ORTE_RML_NON_PERSISTENT,
                                                           recv_direct_msgs,
                                                           NULL))) {
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
            /* record the request for future use */
            request = OBJ_NEW(orcm_pnp_pending_request_t);
            request->tag = tag;
            request->cbfunc_buf = cbfunc;
            opal_list_append(&group->requests, &request->super);
            /* ensure we are listening on all previously-known channels */
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
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:register_input_buffer adding group app %s version %s release %s tag %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             app, (NULL == version) ? "NULL" : version,
                             (NULL == release) ? "NULL" : release, tag));
        
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
    opal_buffer_t buf;
    int8_t flag;
    int32_t cnt;
    int sz;
    
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
    /* make a tmp buffer */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    /* flag the buffer as containing iovecs */
    flag = 0;
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &flag, 1, OPAL_INT8))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    /* pass the target PNP tag */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &tag, 1, ORCM_PNP_TAG_T))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    /* pack the number of iovecs */
    cnt = count;
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &cnt, 1, OPAL_INT32))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }    
    
    /* pack each iovec into a buffer in prep for sending
     * so we can recreate the array at the other end
     */
    for (sz=0; sz < count; sz++) {
        /* pack the size */
        cnt = msg[sz].iov_len;
        if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &cnt, 1, OPAL_INT32))) {
            ORTE_ERROR_LOG(ret);
            return ret;
        }        
        /* pack the bytes */
        if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &(msg[sz].iov_base), cnt, OPAL_UINT8))) {
            ORTE_ERROR_LOG(ret);
            return ret;
        }        
    }
    /* send the msg */
    if (0 > (ret = orte_rml.send_buffer(recipient, &buf, ORTE_RML_TAG_MULTICAST_DIRECT, 0))) {
        ORTE_ERROR_LOG(ret);
    }
    OBJ_DESTRUCT(&buf);
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
    opal_buffer_t *buf;
    int8_t flag;
    int32_t cnt;
    int sz;
    
    send = OBJ_NEW(orcm_pnp_send_t);
    send->tag = tag;
    send->msg = msg;
    send->count = count;
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
    /* make a tmp buffer */
    buf = OBJ_NEW(opal_buffer_t);
    /* flag the buffer as containing iovecs */
    flag = 0;
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &flag, 1, OPAL_INT8))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }    
    /* pass the target tag */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &tag, 1, ORCM_PNP_TAG_T))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }    
    /* pack the number of iovecs */
    cnt = count;
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &cnt, 1, OPAL_INT32))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }    
    
    /* pack each iovec into a buffer in prep for sending
     * so we can recreate the array at the other end
     */
    for (sz=0; sz < count; sz++) {
        /* pack the size */
        cnt = msg[sz].iov_len;
        if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &cnt, 1, OPAL_INT32))) {
            ORTE_ERROR_LOG(ret);
            return ret;
        }        
        /* pack the bytes */
        if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &(msg[sz].iov_base), cnt, OPAL_UINT8))) {
            ORTE_ERROR_LOG(ret);
            return ret;
        }        
    }
    /* send the msg */
    if (ORCM_SUCCESS != (ret = orte_rml.send_buffer_nb(recipient, buf,
                                                       ORTE_RML_TAG_MULTICAST_DIRECT, 0,
                                                       rml_callback_buffer, send))) {
        ORTE_ERROR_LOG(ret);
    }
    return ret;
}

static int default_output_buffer(orte_process_name_t *recipient,
                                 orcm_pnp_tag_t tag,
                                 opal_buffer_t *buffer)
{
    int ret;
    opal_buffer_t buf;
    int8_t flag;

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
    /* make a tmp buffer */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    /* flag that we sent a buffer */
    flag = 1;
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &flag, 1, OPAL_INT8))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }    
    /* pass the target PNP tag */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &tag, 1, ORCM_PNP_TAG_T))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }    
    /* copy the payload */
    if (ORTE_SUCCESS != (ret = opal_dss.copy_payload(&buf, buffer))) {
        ORTE_ERROR_LOG(ret);
        OBJ_DESTRUCT(&buf);
        return ret;
    }
    if (0 > (ret = orte_rml.send_buffer(recipient, &buf, ORTE_RML_TAG_MULTICAST_DIRECT, 0))) {
        ORTE_ERROR_LOG(ret);
    }
    OBJ_DESTRUCT(&buf);
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
    opal_buffer_t *buf;
    int8_t flag;

    send = OBJ_NEW(orcm_pnp_send_t);
    send->tag = tag;
    send->buffer = buffer;
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
    /* make a tmp buffer */
    buf = OBJ_NEW(opal_buffer_t);
    /* flag that we sent a buffer */
    flag = 1;
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &flag, 1, OPAL_INT8))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }    
    /* pass the target tag */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &tag, 1, ORCM_PNP_TAG_T))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }    
    /* copy the payload */
    if (ORTE_SUCCESS != (ret = opal_dss.copy_payload(buf, buffer))) {
        ORTE_ERROR_LOG(ret);
        OBJ_RELEASE(buf);
        return ret;
    }
    if (ORCM_SUCCESS != (ret = orte_rml.send_buffer_nb(recipient, buf,
                                                       ORTE_RML_TAG_MULTICAST_DIRECT, 0,
                                                       rml_callback_buffer, send))) {
        ORTE_ERROR_LOG(ret);
    }
    return ret;
}

static orcm_pnp_group_t* get_group(char *app, char *version, char *release)
{
    opal_list_item_t *item;
    orcm_pnp_group_t *group;

    for (item = opal_list_get_first(&groups);
         item != opal_list_get_end(&groups);
         item = opal_list_get_next(item)) {
        group = (orcm_pnp_group_t*)item;
        
        if (0 == strcasecmp(group->app, app) &&
            (NULL == version || 0 == strcasecmp(group->version, version)) &&
            (NULL == release || 0 == strcasecmp(group->release, release))) {
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s pnp:default:found group %s:%s:%s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), app,
                                 (NULL == version) ? "NULL" : version,
                                 (NULL == release) ? "NULL" : release));
            return group;
        }
    }
    
    /* if we get here, then the group wasn't found - so
     * create it
     */
    group = OBJ_NEW(orcm_pnp_group_t);
    group->app = strdup(app);
    if (NULL != version) {
        group->version = strdup(version);
    }
    if (NULL != release) {
        group->release = strdup(release);
    }
    /* add to the list of groups */
    opal_list_append(&groups, &group->super);

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:defined new group %s:%s:%s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release));
    return group;
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
    
    /* destruct the threading support */
    OBJ_DESTRUCT(&recvs);
    opal_event_del(&ready);
    close(ready_fd[0]);
    processing = false;
    OBJ_DESTRUCT(&lock);

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
    
    /* if the leader is wildcard, then just deliver the message
     * with no further overhead
     */
    if (ORCM_SOURCE_WILDCARD == grp->leader) {
        cbfunc(ORCM_SUCCESS, sender, tag, msg, count, NULL);
        /* update the msg number */
        src->last_msg_num = seq_num;
        return;
    }
    
    /* if we haven't set a leader for this group, do so now */
    if (NULL == grp->leader) {
        grp->leader = src;
    }
    
    /* see if the leader has failed */
    if (orcm_leader.has_leader_failed(grp, src, seq_num)) {
        /* leader has failed - get new one */
        if (ORCM_SUCCESS != (rc = orcm_leader.select_leader(grp))) {
            ORTE_ERROR_LOG(rc);
        }
    }
    
    /* if this data came from the leader, deliver it */
    if (NULL != grp->leader && src == grp->leader) {
        cbfunc(ORCM_SUCCESS, sender, tag, msg, count, NULL);
    }
    
    /* update the msg number */
    src->last_msg_num = seq_num;
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
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received input buffer found grp",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
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
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received input buffer found sender %s in grp",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));
    
    /* if the leader is wildcard, then just deliver the message
     * with no further overhead
     */
    if (ORCM_SOURCE_WILDCARD == grp->leader) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received input buffer wildcard leader - delivering msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        
        cbfunc(ORCM_SUCCESS, sender, tag, buf, NULL);
        /* update the msg number */
        src->last_msg_num = seq_num;
        return;
    }
    
    /* if we haven't set a leader for this group, do so now */
    if (NULL == grp->leader) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received input buffer setting initial leader to %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(sender)));
        grp->leader = src;
    }
    
    /* see if the leader has failed */
    if (orcm_leader.has_leader_failed(grp, src, seq_num)) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received input buffer leader failed",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        
        /* leader has failed - get new one */
        if (ORCM_SUCCESS != (rc = orcm_leader.select_leader(grp))) {
            ORTE_ERROR_LOG(rc);
        }
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received input buffer setting new leader to %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&(grp->leader->name))));
    }
    
    /* if this data came from the leader, deliver it */
    if (NULL != grp->leader && src == grp->leader) {
        cbfunc(ORCM_SUCCESS, sender, tag, buf, NULL);
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
    
    OBJ_RELEASE(send);
}

static void rml_callback_buffer(int status,
                                orte_process_name_t* sender,
                                opal_buffer_t* buffer,
                                orte_rml_tag_t tag,
                                void* cbdata)
{
    orcm_pnp_send_t *send = (orcm_pnp_send_t*)cbdata;
    
    /* release the scratch buffer */
    OBJ_RELEASE(buffer);
    /* do any required callbacks */
    if (NULL != send->cbfunc_buf) {
        send->cbfunc_buf(status, ORTE_PROC_MY_NAME, send->tag, send->buffer, send->cbdata);
    } else if (NULL != send->cbfunc) {
        send->cbfunc(status, ORTE_PROC_MY_NAME, send->tag, send->msg, send->count, send->cbdata);
    }
    OBJ_RELEASE(send);
}

static void process_direct_msgs(int fd, short event, void *cbdata)
{
    orte_msg_packet_t *msgpkt;
    orte_jobid_t job;
    int rc;
    orte_std_cntr_t cnt;
    opal_list_item_t *itmgrp, *itmsrc, *itmrecv, *itmreq;
    int dump[128];
    orte_process_name_t sender;
    orcm_pnp_tag_t tag;
    opal_buffer_t *buf, recvd_buf;
    opal_list_item_t *item;
    orcm_pnp_group_t *group;
    orcm_pnp_source_t *source;
    int8_t flag;
    struct iovec *iovec_array=NULL;
    int32_t iovec_count=0, i, sz, n;
    orcm_pnp_pending_request_t *request;
    
    OPAL_OUTPUT_VERBOSE((5, orcm_pnp_base.output,
                         "%s pnp:default: processing direct msg",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    OPAL_THREAD_LOCK(&lock);
    
    /* tag that we are processing the list */
    processing = true;
    
    /* clear the file descriptor to stop the event from refiring */
    read(fd, &dump, sizeof(dump));
    
    while (NULL != (itmrecv = opal_list_remove_first(&recvs))) {
        msgpkt = (orte_msg_packet_t*)itmrecv;
        
        sender.jobid = msgpkt->sender.jobid;
        sender.vpid = msgpkt->sender.vpid;
        buf = msgpkt->buffer;
        
        /* unpack the flag */
        n=1;
        if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &flag, &n, OPAL_INT8))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(msgpkt);
            continue;
        }
        
        /* unpack the intended tag for this message */
        n=1;
        if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &tag, &n, ORCM_PNP_TAG_T))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(msgpkt);
            continue;
        }
        
        /* locate this sender */
        for (itmgrp = opal_list_get_first(&groups);
             itmgrp != opal_list_get_end(&groups);
             itmgrp = opal_list_get_next(itmgrp)) {
            group = (orcm_pnp_group_t*)itmgrp;
            
            for (itmsrc = opal_list_get_first(&group->members);
                 itmsrc != opal_list_get_end(&group->members);
                 itmsrc = opal_list_get_next(itmsrc)) {
                source = (orcm_pnp_source_t*)itmsrc;
                
                if (sender.jobid == source->name.jobid &&
                    sender.vpid == source->name.vpid) {
                    /* found it - since a source can only be a
                     * member of one group, we only need to
                     * unpack the msg once
                     */
                    if (1 == flag) {
                        /* buffer was included */
                        OBJ_CONSTRUCT(&recvd_buf, opal_buffer_t);
                        /* copy the payload */
                        if (ORTE_SUCCESS != (rc = opal_dss.copy_payload(&recvd_buf, buf))) {
                            ORTE_ERROR_LOG(rc);
                            OBJ_DESTRUCT(&recvd_buf);
                            goto NEXT;
                        }
                        /* loop through the recorded recv requests on this
                         * group to find the specified tag so we can get
                         * the associated callback function
                         */
                        for (itmreq = opal_list_get_first(&group->requests);
                             itmreq != opal_list_get_end(&group->requests);
                             itmreq = opal_list_get_next(itmreq)) {
                            request = (orcm_pnp_pending_request_t*)itmreq;
                            
                            if (request->tag == tag &&
                                NULL != request->cbfunc_buf) {
                                /* deliver it */
                                request->cbfunc_buf(ORCM_SUCCESS, &sender, tag, &recvd_buf, NULL);
                                break;
                            }
                        }
                        /* release scratch buffer */
                        OBJ_DESTRUCT(&recvd_buf);
                        /* done */
                        goto NEXT;
                    } else if (0 == flag) {
                        /* iovecs included and we still need to unpack it - get
                         * the number of iovecs in the buffer
                         */
                        n=1;
                        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &iovec_count, &n, OPAL_INT32))) {
                            ORTE_ERROR_LOG(rc);
                            goto NEXT;
                        }
                        /* malloc the required space */
                        iovec_array = (struct iovec *)malloc(iovec_count * sizeof(struct iovec));
                        /* unpack the iovecs */
                        for (i=0; i < iovec_count; i++) {
                            /* unpack the number of bytes in this iovec */
                            n=1;
                            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, &sz, &n, OPAL_INT32))) {
                                ORTE_ERROR_LOG(rc);
                                goto NEXT;
                            }
                            /* allocate the space */
                            iovec_array[i].iov_base = (uint8_t*)malloc(sz);
                            iovec_array[i].iov_len = sz;
                            /* unpack the data */
                            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buf, iovec_array[i].iov_base, &sz, OPAL_UINT8))) {
                                ORTE_ERROR_LOG(rc);
                                goto NEXT;
                            }                    
                        }
                        /* loop through the recorded recv requests on this
                         * group to find the specified tag so we can get
                         * the associated callback function
                         */
                        for (itmreq = opal_list_get_first(&group->requests);
                             itmreq != opal_list_get_end(&group->requests);
                             itmreq = opal_list_get_next(itmreq)) {
                            request = (orcm_pnp_pending_request_t*)itmreq;
                            
                            if (request->tag == tag &&
                                NULL != request->cbfunc) {
                                /* deliver it */
                                request->cbfunc(ORCM_SUCCESS, &sender, tag, iovec_array, iovec_count, NULL);
                                break;
                            }
                        }                        
                        /* release the memory */
                        for (i=0; i < iovec_count; i++) {
                            free(iovec_array[i].iov_base);
                        }
                        free(iovec_array);                        
                    }
                    goto NEXT;
                }
            }
        }
    NEXT:
        OBJ_RELEASE(msgpkt);
    }
    
    /* reset the event */
    processing = false;
    opal_event_add(&ready, 0);
    
    /* release the thread */
    OPAL_THREAD_UNLOCK(&lock);
}

static void recv_direct_msgs(int status, orte_process_name_t* sender,
                             opal_buffer_t* buffer, orte_rml_tag_t tag,
                             void* cbdata)
{
    int ret;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:defaultrecvd direct msg from %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));
    
    /* don't process this right away - we need to get out of the recv before
     * we process the message as it may ask us to do something that involves
     * more messaging! Instead, setup an event so that the message gets processed
     * as soon as we leave the recv.
     *
     * The macro makes a copy of the buffer, which we release above - the incoming
     * buffer, however, is NOT released here, although its payload IS transferred
     * to the message buffer for later processing
     */
    ORTE_PROCESS_MESSAGE(&recvs, &lock, processing, ready_fd[1], true, sender, &buffer);

    /* reissue the recv */
    if (ORTE_SUCCESS != (ret = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD,
                                                       ORTE_RML_TAG_MULTICAST_DIRECT,
                                                       ORTE_RML_NON_PERSISTENT,
                                                       recv_direct_msgs,
                                                       NULL))) {
        ORTE_ERROR_LOG(ret);
    }
}

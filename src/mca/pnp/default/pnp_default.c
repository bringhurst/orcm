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
#include "opal/class/opal_list.h"
#include "opal/class/opal_pointer_array.h"
#include "opal/util/output.h"
#include "opal/threads/condition.h"
#include "opal/threads/mutex.h"
#include "opal/threads/threads.h"

#include "orte/mca/rmcast/rmcast.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rml/rml.h"
#include "orte/runtime/orte_globals.h"

#include "mca/leader/leader.h"
#include "runtime/runtime.h"

#include "mca/pnp/pnp.h"
#include "mca/pnp/base/private.h"
#include "mca/pnp/default/pnp_default.h"

/* API functions */

static int default_init(void);
static int announce(char *app, char *version, char *release,
                    orcm_pnp_announce_fn_t cbfunc);
static orcm_pnp_channel_t open_channel(char *app, char *version, char *release);
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
static int default_output(orcm_pnp_channel_t channel,
                          orte_process_name_t *recipient,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg, int count);
static int default_output_nb(orcm_pnp_channel_t channel,
                             orte_process_name_t *recipient,
                             orcm_pnp_tag_t tag,
                             struct iovec *msg, int count,
                             orcm_pnp_callback_fn_t cbfunc,
                             void *cbdata);
static int default_output_buffer(orcm_pnp_channel_t channel,
                                 orte_process_name_t *recipient,
                                 orcm_pnp_tag_t tag,
                                 opal_buffer_t *buffer);
static int default_output_buffer_nb(orcm_pnp_channel_t channel,
                                    orte_process_name_t *recipient,
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
    open_channel,
    register_input,
    register_input_buffer,
    deregister_input,
    default_output,
    default_output_nb,
    default_output_buffer,
    default_output_buffer_nb,
    define_new_tag,
    default_finalize
};

/* Local functions */
static void recv_announcements(int status,
                               orte_process_name_t *sender,
                               orcm_pnp_tag_t tag,
                               opal_buffer_t *buf, void *cbdata);
static void recv_inputs(int status,
                        orte_rmcast_channel_t channel,
                        orte_rmcast_tag_t tag,
                        orte_process_name_t *sender,
                        struct iovec *msg, int count, void *cbdata);
static void recv_input_buffers(int status,
                               orte_rmcast_channel_t channel,
                               orte_rmcast_tag_t tag,
                               orte_process_name_t *sender,
                               opal_buffer_t *buf, void *cbdata);
static int pack_announcement(opal_buffer_t *buf);

static void rmcast_callback_buffer(int status,
                                   orte_rmcast_channel_t channel,
                                   orte_rmcast_tag_t tag,
                                   orte_process_name_t *sender,
                                   opal_buffer_t *buf, void* cbdata);

static void rmcast_callback(int status,
                            orte_rmcast_channel_t channel,
                            orte_rmcast_tag_t tag,
                            orte_process_name_t *sender,
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

static void* recv_messages(opal_object_t *obj);

static void recv_direct_msgs(int status, orte_process_name_t* sender,
                             opal_buffer_t* buffer, orte_rml_tag_t tag,
                             void* cbdata);

static orcm_pnp_channel_tracker_t* get_channel(char *app,
                                               char *version,
                                               char *release);

static orcm_pnp_channel_tracker_t* find_channel(orcm_pnp_channel_t channel);

static void setup_recv_request(orcm_pnp_channel_tracker_t *tracker,
                               orcm_pnp_tag_t tag,
                               orcm_pnp_callback_fn_t cbfunc,
                               orcm_pnp_callback_buffer_fn_t cbfunc_buf);

/* Local variables */
static opal_pointer_array_t groups, channels;
static int32_t packet_number = 0;
static bool recv_on = false;
static char *my_app = NULL;
static char *my_version = NULL;
static char *my_release = NULL;
static void* my_announce_cbfunc = NULL;
static opal_list_t recvs;
static orte_rmcast_channel_t my_channel;
static orcm_pnp_channel_t my_pnp_channels = ORCM_PNP_DYNAMIC_CHANNELS;

/* local thread support */
static opal_mutex_t lock, recvlock;
static opal_condition_t cond, recvcond;
static bool active = false;
static opal_thread_t recv_thread;

static int default_init(void)
{
    int ret;
    orcm_pnp_group_t *group;
    orcm_pnp_pending_request_t *request;
    orcm_pnp_source_t *src;
    orcm_pnp_channel_tracker_t *tracker;
    
    /* init the array of known application groups */
    OBJ_CONSTRUCT(&groups, opal_pointer_array_t);
    opal_pointer_array_init(&groups, 8, INT_MAX, 8);
    
    /* init the array of channels */
    OBJ_CONSTRUCT(&channels, opal_pointer_array_t);
    opal_pointer_array_init(&channels, 8, INT_MAX, 8);
    
    /* setup the threading support */
    OBJ_CONSTRUCT(&lock, opal_mutex_t);
    OBJ_CONSTRUCT(&cond, opal_condition_t);
    OBJ_CONSTRUCT(&recvlock, opal_mutex_t);
    OBJ_CONSTRUCT(&recvcond, opal_condition_t);
    OBJ_CONSTRUCT(&recvs, opal_list_t);
    
    /* record my channel */
    my_channel = orte_rmcast.query_channel();
    
    /* setup a recv to catch any announcements */
    if (!recv_on) {
        /* setup the respective public address channel */
        if (OPENRCM_PROC_IS_MASTER || OPENRCM_PROC_IS_DAEMON || OPENRCM_PROC_IS_TOOL) {
            /* setup a group */
            group = OBJ_NEW(orcm_pnp_group_t);
            group->app = strdup("ORCM_SYSTEM");
            group->channel = ORTE_RMCAST_SYS_CHANNEL;
            /* add to the list of groups */
            opal_pointer_array_add(&groups, group);
            /* add this channel to our list */
            tracker = OBJ_NEW(orcm_pnp_channel_tracker_t);
            tracker->app = strdup(group->app);
            tracker->channel = ORCM_PNP_SYS_CHANNEL;
            opal_pointer_array_add(&channels, tracker);
            OBJ_RETAIN(group);
            opal_pointer_array_add(&tracker->groups, group);
            /* record the request for future use */
            request = OBJ_NEW(orcm_pnp_pending_request_t);
            request->tag = ORTE_RMCAST_TAG_WILDCARD;
            request->cbfunc_buf = recv_announcements;
            opal_pointer_array_set_item(&group->requests, request->tag, request);
            /* open a channel to this group - will just return if
             * the channel already exists
             */
            if (ORCM_SUCCESS != (ret = orte_rmcast.open_channel(&group->channel,
                                                                group->app,
                                                                NULL, -1, NULL,
                                                                ORTE_RMCAST_RECV))) {
                ORTE_ERROR_LOG(ret);
                return ret;
            }
            /* setup to listen to it - will just return if we already are */
            if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(group->channel, request->tag,
                                                                  ORTE_RMCAST_PERSISTENT,
                                                                  recv_input_buffers, NULL))) {
                ORTE_ERROR_LOG(ret);
                return ret;
            }
            
        } else if (OPENRCM_PROC_IS_APP) {
            /* setup a group */
            group = OBJ_NEW(orcm_pnp_group_t);
            group->app = strdup("ORCM_APP_ANNOUNCE");
            group->channel = ORTE_RMCAST_APP_PUBLIC_CHANNEL;
            /* add to the list of groups */
            opal_pointer_array_add(&groups, group);
            /* add this channel to our list */
            tracker = OBJ_NEW(orcm_pnp_channel_tracker_t);
            tracker->app = strdup(group->app);
            tracker->channel = ORCM_PNP_SYS_CHANNEL;
            opal_pointer_array_add(&channels, tracker);
            OBJ_RETAIN(group);
            opal_pointer_array_add(&tracker->groups, group);
            /* record the request for future use */
            request = OBJ_NEW(orcm_pnp_pending_request_t);
            request->tag = ORTE_RMCAST_TAG_ANNOUNCE;
            request->cbfunc_buf = recv_announcements;
            opal_pointer_array_set_item(&group->requests, request->tag, request);
            
            /* open the channel */
            if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(group->channel, request->tag,
                                                                  ORTE_RMCAST_PERSISTENT,
                                                                  recv_input_buffers, NULL))) {
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
        }
        recv_on = true;
    }
    
    /* setup the message processing thread */
    OBJ_CONSTRUCT(&recv_thread, opal_thread_t);
    recv_thread.t_run = recv_messages;
    active = true;
    if (ORCM_SUCCESS != (ret = opal_thread_start(&recv_thread))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    
    return ORCM_SUCCESS;
}

static int announce(char *app, char *version, char *release,
                    orcm_pnp_announce_fn_t cbfunc)
{
    int ret;
    opal_buffer_t buf;
    orcm_pnp_group_t *group;
    orcm_pnp_channel_tracker_t *tracker;
    
    /* bozo check */
    if (NULL == app || NULL == version || NULL == release) {
        ORTE_ERROR_LOG(ORCM_ERR_BAD_PARAM);
        return ORCM_ERR_BAD_PARAM;
    }
    
    /* ensure we don't do this more than once - could be called
     * from multiple threads if someone goofs
     */
    OPAL_THREAD_LOCK(&lock);
    if (NULL != my_app) {
        /* must have been called before */
        OPAL_THREAD_UNLOCK(&lock);
        return ORCM_SUCCESS;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:announce app %s version %s release %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         app, version, release));
    
    /* retain a local record of my info */
    my_app = strdup(app);
    my_version = strdup(version);
    my_release = strdup(release);
    
    /* retain the callback function */
    my_announce_cbfunc = cbfunc;
    
    /* add to the list of groups */
    group = OBJ_NEW(orcm_pnp_group_t);
    group->app = strdup(my_app);
    group->version = strdup(my_version);
    group->release = strdup(my_release);
    group->channel = my_channel;
    opal_pointer_array_add(&groups, group);
    
    /* add it to our list of channels */
    tracker = OBJ_NEW(orcm_pnp_channel_tracker_t);
    tracker->app = strdup(my_app);
    tracker->version = strdup(my_version);
    tracker->release = strdup(my_release);
    tracker->channel = ORCM_PNP_GROUP_OUTPUT_CHANNEL;
    opal_pointer_array_add(&channels, tracker);
    OBJ_RETAIN(group);
    opal_pointer_array_add(&tracker->groups, group);

    /* no need to hold the lock any further */
    OPAL_THREAD_UNLOCK(&lock);
    
    /* assemble the announcement message */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    
    /* pack the common elements */
    if (ORCM_SUCCESS != (ret = pack_announcement(&buf))) {
        ORTE_ERROR_LOG(ret);
        OBJ_DESTRUCT(&buf);
        OPAL_THREAD_UNLOCK(&lock);
        return ret;
    }
    /* this is an original announcement, so we are responding to nobody */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, ORTE_NAME_INVALID, 1, ORTE_NAME))) {
        ORTE_ERROR_LOG(ret);
        OBJ_DESTRUCT(&buf);
        OPAL_THREAD_UNLOCK(&lock);
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

static orcm_pnp_channel_t open_channel(char *app, char *version, char *release)
{
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *grp;
    orcm_pnp_pending_request_t *request;
    int i, j, rc;
    
    /* bozo check */
    if (NULL == app) {
        return ORCM_PNP_INVALID_CHANNEL;
    }
    
    /* protect the global arrays */
    OPAL_THREAD_LOCK(&lock);

    /* see if we already have this channel - automatically
     * creates it if not
     */
    tracker = get_channel(app, version, release);
    
    /* if this channel already existed, it may have groups in it - so we
     * need to loop across them and open a channel, if not already done
     */
    for (i=0; i < tracker->groups.size; i++) {
        if (NULL == (grp = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
            continue;
        }
        /* open a channel to this group - will just return if
         * the channel already exists
         */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:open_channel opening channel %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), grp->channel));
        if (ORCM_SUCCESS != (rc = orte_rmcast.open_channel(&grp->channel,
                                                           grp->app, NULL, -1, NULL,
                                                           ORTE_RMCAST_BIDIR))) {
            ORTE_ERROR_LOG(rc);
            break;
        }
        /* if there are any pending requests, setup a recv for them */
        /* do we want to listen to this group? */
        for (j=0; j < grp->requests.size; j++) {
            if (NULL == (request = (orcm_pnp_pending_request_t*)opal_pointer_array_get_item(&grp->requests, j))) {
                continue;
            }
            /* open the channel */
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s pnp:default:open_channel setup recv for channel %d",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), grp->channel));
            /* setup the recvs */
            if (ORTE_SUCCESS != (rc = orte_rmcast.recv_nb(grp->channel,
                                                          ORTE_RMCAST_TAG_WILDCARD,
                                                          ORTE_RMCAST_PERSISTENT,
                                                          recv_inputs, NULL))) {
                ORTE_ERROR_LOG(rc);
                break;
            }
            if (ORTE_SUCCESS != (rc = orte_rmcast.recv_buffer_nb(grp->channel,
                                                                 ORTE_RMCAST_TAG_WILDCARD,
                                                                 ORTE_RMCAST_PERSISTENT,
                                                                 recv_input_buffers, NULL))) {
                ORTE_ERROR_LOG(rc);
                break;
            }
            /* we only need to do this once - we'll sort out the
             * deliveries when we recv a message
             */
            break;
        }
    }
    
    OPAL_THREAD_UNLOCK(&lock);
    
    return tracker->channel;
}

static int register_input(char *app,
                          char *version,
                          char *release,
                          orcm_pnp_tag_t tag,
                          orcm_pnp_callback_fn_t cbfunc)
{
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *grp;
    int i, ret=ORCM_SUCCESS;
    
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
    
    /* since we are modifying global lists, lock
     * the thread
     */
    OPAL_THREAD_LOCK(&lock);
    
    /* get a tracker object for this triplet - creates
     * it if one doesn't already exist
     */
    tracker = get_channel(app, version, release);
    
    /* record the request - will just return if this request
     * already exists
     */
    setup_recv_request(tracker, tag, cbfunc, NULL);
    
    /* ensure we are listening to any pre-known groups */
    for (i=0; i < tracker->groups.size; i++) {
        if (NULL == (grp = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
            continue;
        }
        if (ORTE_RMCAST_INVALID_CHANNEL == grp->channel) {
            /* don't know this one yet */
            continue;
        }
        /* open a channel to this group - will just return if
         * the channel already exists
         */
        if (ORCM_SUCCESS != (ret = orte_rmcast.open_channel(&grp->channel,
                                                            grp->app,
                                                            NULL, -1, NULL,
                                                            ORTE_RMCAST_BIDIR))) {
            ORTE_ERROR_LOG(ret);
            goto cleanup;
        }
        /* setup to listen to it - will just return if we already are */
        if (ORTE_SUCCESS != (ret = orte_rmcast.recv_nb(grp->channel, tag,
                                                       ORTE_RMCAST_PERSISTENT,
                                                       recv_inputs, cbfunc))) {
            ORTE_ERROR_LOG(ret);
            goto cleanup;
        }
    }
    
cleanup:
    /* clear the thread */
    OPAL_THREAD_UNLOCK(&lock);
    
    return ret;
}

static int register_input_buffer(char *app,
                                 char *version,
                                 char *release,
                                 orcm_pnp_tag_t tag,
                                 orcm_pnp_callback_buffer_fn_t cbfunc)
{
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *grp;
    int i, ret=ORCM_SUCCESS;
    
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
    
    /* since we are modifying global lists, lock
     * the thread
     */
    OPAL_THREAD_LOCK(&lock);
    
    /* get a tracker object for this triplet - creates
     * it if one doesn't already exist
     */
    tracker = get_channel(app, version, release);
    
    /* record the request - will just return if this request
     * already exists
     */
    setup_recv_request(tracker, tag, NULL, cbfunc);
    
    /* ensure we are listening to any pre-known groups */
    for (i=0; i < tracker->groups.size; i++) {
        if (NULL == (grp = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
            continue;
        }
        if (ORTE_RMCAST_INVALID_CHANNEL == grp->channel) {
            /* don't know this one yet */
            continue;
        }
        /* open a channel to this group - will just return if
         * the channel already exists
         */
        if (ORCM_SUCCESS != (ret = orte_rmcast.open_channel(&grp->channel,
                                                            grp->app,
                                                            NULL, -1, NULL,
                                                            ORTE_RMCAST_BIDIR))) {
            ORTE_ERROR_LOG(ret);
            goto cleanup;
        }
        /* setup to listen to it - will just return if we already are */
        if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(grp->channel, tag,
                                                              ORTE_RMCAST_PERSISTENT,
                                                              recv_input_buffers, cbfunc))) {
            ORTE_ERROR_LOG(ret);
            goto cleanup;
        }
    }
    
cleanup:
    /* clear the thread */
    OPAL_THREAD_UNLOCK(&lock);
    
    return ret;
}

static int deregister_input(char *app,
                            char *version,
                            char *release,
                            orcm_pnp_tag_t tag)
{
    orcm_pnp_group_t *group;
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_pending_request_t *request;
    int i, j, k;
    
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
    
    /* since we are modifying global lists, lock
     * the thread
     */
    OPAL_THREAD_LOCK(&lock);
    
    /* get the tracker object for this triplet */
    tracker = get_channel(app, version, release);

    /* cycle through requests to match this one */
    for (i=0; i < tracker->requests.size; i++) {
        if (NULL == (request = (orcm_pnp_pending_request_t*)opal_pointer_array_get_item(&tracker->requests, i))) {
            continue;
        }
        if (ORCM_PNP_TAG_WILDCARD == tag || tag == request->tag) {
            OBJ_RELEASE(request);
            for (j=0; j < tracker->groups.size; j++) {
                if (NULL == (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, j))) {
                    continue;
                }
                if (ORCM_PNP_TAG_WILDCARD == tag) {
                    for (k=0; k < group->requests.size; k++) {
                        if (NULL != (request = (orcm_pnp_pending_request_t*)opal_pointer_array_get_item(&group->requests, k))) {
                            OBJ_RELEASE(request);
                        }
                    }
                } else {
                    if (NULL != (request = (orcm_pnp_pending_request_t*)opal_pointer_array_get_item(&group->requests, tag))) {
                        OBJ_RELEASE(request);
                    }
                }
            }
        }
    }

    /* clear the thread */
    OPAL_THREAD_UNLOCK(&lock);
    
    return ORCM_SUCCESS;
}

static int default_output(orcm_pnp_channel_t channel,
                          orte_process_name_t *recipient,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg, int count)
{
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *grp;
    int i, ret;
    opal_buffer_t buf;
    int8_t flag;
    int32_t cnt;
    int sz;
    
    OPAL_THREAD_LOCK(&lock);

    /* find the channel */
    if (NULL == (tracker = find_channel(channel))) {
        /* don't know this channel - throw bits on floor */
        OPAL_THREAD_UNLOCK(&lock);
        return ORCM_SUCCESS;
    }
    
    /* if this is intended for everyone who might be listening on this channel,
     * multicast it to all groups in this channel
     */
    if (NULL == recipient ||
        (ORTE_JOBID_WILDCARD == recipient->jobid &&
         ORTE_VPID_WILDCARD == recipient->vpid)) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:sending multicast of %d iovecs to tag %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), count, tag));
        
        for (i=0; i < tracker->groups.size; i++) {
            if (NULL == (grp = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
                continue;
            }
            if (ORTE_RMCAST_INVALID_CHANNEL == grp->channel) {
                /* don't know this one yet */
                continue;
            }
            /* send the iovecs to the channel */
            if (ORCM_SUCCESS != (ret = orte_rmcast.send(grp->channel, tag, msg, count))) {
                ORTE_ERROR_LOG(ret);
            }
        }
        OPAL_THREAD_UNLOCK(&lock);
        return ORCM_SUCCESS;
    }
    OPAL_THREAD_UNLOCK(&lock);

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
    /* pack our channel so the recipient can figure out who it came from */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &my_channel, 1, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }    
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
        if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, msg[sz].iov_base, cnt, OPAL_UINT8))) {
            ORTE_ERROR_LOG(ret);
            return ret;
        }        
    }
    /* send the msg */
    if (0 > (ret = orte_rml.send_buffer(recipient, &buf, ORTE_RML_TAG_MULTICAST_DIRECT, 0))) {
        ORTE_ERROR_LOG(ret);
    } else {
        ret = ORCM_SUCCESS;
    }
    OBJ_DESTRUCT(&buf);
    return ret;
}

static int default_output_nb(orcm_pnp_channel_t channel,
                             orte_process_name_t *recipient,
                             orcm_pnp_tag_t tag,
                             struct iovec *msg, int count,
                             orcm_pnp_callback_fn_t cbfunc,
                             void *cbdata)
{
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *grp;
    int i, ret;
    orcm_pnp_send_t *send;
    opal_buffer_t *buf;
    int8_t flag;
    int32_t cnt;
    int sz;
    
    OPAL_THREAD_LOCK(&lock);
    
    /* find the channel */
    if (NULL == (tracker = find_channel(channel))) {
        /* don't know this channel - throw bits on floor */
        OPAL_THREAD_UNLOCK(&lock);
        return ORCM_SUCCESS;
    }
    
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
            
            for (i=0; i < tracker->groups.size; i++) {
                if (NULL == (grp = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
                    continue;
                }
                if (ORTE_RMCAST_INVALID_CHANNEL == grp->channel) {
                    /* don't know this one yet */
                    continue;
                }
                /* maintain accounting */
                OBJ_RETAIN(send);
                /* send the iovecs to the channel */
                if (ORCM_SUCCESS != (ret = orte_rmcast.send_nb(grp->channel, tag,
                                                               msg, count, rmcast_callback, send))) {
                    ORTE_ERROR_LOG(ret);
                }
            }
            /* correct accounting */
            OBJ_RELEASE(send);
            OPAL_THREAD_UNLOCK(&lock);
            return ORCM_SUCCESS;
        }
    OPAL_THREAD_UNLOCK(&lock);
    
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
    /* pack our channel so the recipient can figure out who it came from */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_channel, 1, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }    
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
        if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, msg[sz].iov_base, cnt, OPAL_UINT8))) {
            ORTE_ERROR_LOG(ret);
            return ret;
        }        
    }
    /* send the msg */
    if (0 > (ret = orte_rml.send_buffer_nb(recipient, buf,
                                           ORTE_RML_TAG_MULTICAST_DIRECT, 0,
                                           rml_callback_buffer, send))) {
        ORTE_ERROR_LOG(ret);
    } else {
        ret = ORCM_SUCCESS;
    }
    return ret;
}

static int default_output_buffer(orcm_pnp_channel_t channel,
                                 orte_process_name_t *recipient,
                                 orcm_pnp_tag_t tag,
                                 opal_buffer_t *buffer)
{
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *grp;
    int i, ret;
    opal_buffer_t buf;
    int8_t flag;
    
    OPAL_THREAD_LOCK(&lock);
    
    /* find the channel */
    if (NULL == (tracker = find_channel(channel))) {
        /* don't know this channel - throw bits on floor */
        OPAL_THREAD_UNLOCK(&lock);
        return ORCM_SUCCESS;
    }
    
    /* if this is intended for everyone who might be listening on this channel,
     * multicast it to all groups in this channel
     */
    if (NULL == recipient ||
        (ORTE_JOBID_WILDCARD == recipient->jobid &&
         ORTE_VPID_WILDCARD == recipient->vpid)) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:sending multicast buffer of %d bytes to channel %d tag %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)buffer->bytes_used,
                             (int)ORTE_RMCAST_GROUP_OUTPUT_CHANNEL, tag));
        
        for (i=0; i < tracker->groups.size; i++) {
            if (NULL == (grp = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
                continue;
            }
            if (ORTE_RMCAST_INVALID_CHANNEL == grp->channel) {
                /* don't know this one yet */
                continue;
            }
            /* send the buffer to my group output channel */
            if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer(grp->channel, tag, buffer))) {
                ORTE_ERROR_LOG(ret);
            }
            OPAL_THREAD_UNLOCK(&lock);
            return ORCM_SUCCESS;
        }
    }            
    OPAL_THREAD_UNLOCK(&lock);
            
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
    /* pack our channel so the recipient can figure out who it came from */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &my_channel, 1, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }    
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
    } else {
        ret = ORCM_SUCCESS;
    }
    OBJ_DESTRUCT(&buf);
    return ret;
}

static int default_output_buffer_nb(orcm_pnp_channel_t channel,
                                    orte_process_name_t *recipient,
                                    orcm_pnp_tag_t tag,
                                    opal_buffer_t *buffer,
                                    orcm_pnp_callback_buffer_fn_t cbfunc,
                                    void *cbdata)
{
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *grp;
    int i, ret;
    orcm_pnp_send_t *send;
    opal_buffer_t *buf;
    int8_t flag;
    
    OPAL_THREAD_LOCK(&lock);
    
    /* find the channel */
    if (NULL == (tracker = find_channel(channel))) {
        /* don't know this channel - throw bits on floor */
        OPAL_THREAD_UNLOCK(&lock);
        return ORCM_SUCCESS;
    }
    
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
            
            for (i=0; i < tracker->groups.size; i++) {
                if (NULL == (grp = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
                    continue;
                }
                if (ORTE_RMCAST_INVALID_CHANNEL == grp->channel) {
                    /* don't know this one yet */
                    continue;
                }
                /* maintain accounting */
                OBJ_RETAIN(send);
                /* send the iovecs to the channel */
                if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer_nb(grp->channel, tag, buffer,
                                                                      rmcast_callback_buffer, send))) {
                    ORTE_ERROR_LOG(ret);
                }
            }
            /* correct accounting */
            OBJ_RELEASE(send);
            OPAL_THREAD_UNLOCK(&lock);
            return ORCM_SUCCESS;
        }
    OPAL_THREAD_UNLOCK(&lock);
    
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
    /* pack our channel so the recipient can figure out who it came from */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_channel, 1, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }    
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
    if (0 > (ret = orte_rml.send_buffer_nb(recipient, buf,
                                           ORTE_RML_TAG_MULTICAST_DIRECT, 0,
                                           rml_callback_buffer, send))) {
        ORTE_ERROR_LOG(ret);
    } else {
        ret = ORCM_SUCCESS;
    }
    return ret;
}

static orcm_pnp_tag_t define_new_tag(void)
{
    return ORCM_PNP_TAG_INVALID;
}

static int default_finalize(void)
{
    int i;
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *group;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default: finalizing",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* cancel the recvs, if active */
    if (recv_on) {
        orte_rmcast.cancel_recv(ORTE_RMCAST_APP_PUBLIC_CHANNEL, ORTE_RMCAST_TAG_ANNOUNCE);
        orte_rml.recv_cancel(ORTE_NAME_WILDCARD, ORTE_RML_TAG_MULTICAST_DIRECT);
        recv_on = false;
    }
    
    /* if the message processing thread is active, deactivate it */
    if (active) {
        active = false;
        /* put a NULL/NULL message on the processing list
         * to tell the thread to exit
         */
        ORCM_PROCESS_PNP_IOVECS(&recvs, &recvlock, &recvcond,
                                NULL, NULL, 0, ORTE_RMCAST_TAG_INVALID,
                                NULL, 0, NULL);
        /* wait for the thread to exit */
        opal_thread_join(&recv_thread, NULL);
    }
    /* destruct the threading support */
    OBJ_DESTRUCT(&recvs);
    OBJ_DESTRUCT(&lock);
    OBJ_DESTRUCT(&cond);
    OBJ_DESTRUCT(&recvlock);
    OBJ_DESTRUCT(&recvcond);
    OBJ_DESTRUCT(&recv_thread);
    
    /* release the array of known application groups */
    for (i=0; i < groups.size; i++) {
        if (NULL != (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&groups, i))) {
            OBJ_RELEASE(group);
        }
    }
    OBJ_DESTRUCT(&groups);
    /* release the array of known channels */
    for (i=0; i < channels.size; i++) {
        if (NULL != (tracker = (orcm_pnp_channel_tracker_t*)opal_pointer_array_get_item(&channels, i))) {
            OBJ_RELEASE(tracker);
        }
    }
    OBJ_DESTRUCT(&channels);
    
    return ORCM_SUCCESS;
}


/****    LOCAL  FUNCTIONS    ****/
static void recv_announcements(int status,
                               orte_process_name_t *fake,
                               orcm_pnp_tag_t tag,
                               opal_buffer_t *buf, void *cbdata)
{
    opal_list_item_t *itm2;
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *group, *grp;
    orcm_pnp_source_t *source;
    char *app, *version, *release;
    orte_process_name_t originator;
    opal_buffer_t ann;
    int rc, n, i, j;
    orcm_pnp_pending_request_t *request;
    orte_rmcast_channel_t output;
    orte_process_name_t sender;
    orcm_pnp_send_t *pkt;
    
    /* unpack the name of the sender */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &sender, &n, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
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
    
    /* get its multicast channel */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &output, &n, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement from group app %s version %s release %s channel %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         app, version, release, output));
    
    /* since we are accessing global lists, lock
     * the thread
     */
    OPAL_THREAD_LOCK(&lock);
    
    /* do we already know this application group? */
    for (i=0; i < groups.size; i++) {
        if (NULL == (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&groups, i))) {
            continue;
        }
        
        /* the triplet must be unique */
        if (0 != strcasecmp(group->app, app)) {
            continue;
        }
        if (0 != strcasecmp(group->version, version)) {
            continue;
        }
        if (NULL != group->release && 0 != strcasecmp(group->release, release)) {
            continue;
        }

        /* yep - know this one */
        free(app);
        free(version);
        free(release);
        /* record the multicast channel it is on */
        group->channel = output;
        goto recvs;
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
    group->channel = output;
    opal_pointer_array_add(&groups, group);
    
    /* check which channels it might belong to */
    for (i=0; i < channels.size; i++) {
        if (NULL == (tracker = (orcm_pnp_channel_tracker_t*)opal_pointer_array_get_item(&channels, i))) {
            continue;
        }
        if (0 != strcasecmp(app, tracker->app)) {
            continue;
        }
        if (NULL != tracker->version && 0 != strcasecmp(tracker->version, version)) {
            continue;
        }
        if (NULL != tracker->release && 0 != strcasecmp(tracker->release, release)) {
            continue;
        }
        /* have a match - add the group */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:adding group %s:%s:%s to tracker %s:%s:%s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             group->app, group->version, group->release,
                             tracker->app, (NULL == tracker->version) ? "NULL" : tracker->version,
                             (NULL == tracker->release) ? "NULL" : tracker->release));
        OBJ_RETAIN(group);
        opal_pointer_array_add(&tracker->groups, group);
        /* open the channel */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:recvd_ann opening channel %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), output));
        /* open a channel to this group - will just return if
         * the channel already exists
         */
        if (ORCM_SUCCESS != (rc = orte_rmcast.open_channel(&output,
                                                           group->app, NULL, -1, NULL,
                                                           ORTE_RMCAST_BIDIR))) {
            ORTE_ERROR_LOG(rc);
            OPAL_THREAD_UNLOCK(&lock);
            return;
        }
        /* add any pending requests associated with this channel */
        for (j=0; j < tracker->requests.size; j++) {
            if (NULL == (request = (orcm_pnp_pending_request_t*)opal_pointer_array_get_item(&tracker->requests, j))) {
                continue;
            }
            if (NULL != opal_pointer_array_get_item(&group->requests, request->tag)) {
                /* already assigned */
                continue;
            }
            OBJ_RETAIN(request);
            opal_pointer_array_set_item(&group->requests, request->tag, request);
        }
    }
    
recvs:
    /* do we want to listen to this group? */
    for (j=0; j < group->requests.size; j++) {
        if (NULL == (request = (orcm_pnp_pending_request_t*)opal_pointer_array_get_item(&group->requests, j))) {
            continue;
        }
        /* open the channel */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:recvd_ann setup recv for channel %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), output));
        /* setup the recvs */
        if (ORTE_SUCCESS != (rc = orte_rmcast.recv_nb(output,
                                                      ORTE_RMCAST_TAG_WILDCARD,
                                                      ORTE_RMCAST_PERSISTENT,
                                                      recv_inputs, NULL))) {
            ORTE_ERROR_LOG(rc);
            OPAL_THREAD_UNLOCK(&lock);
            return;
        }
        if (ORTE_SUCCESS != (rc = orte_rmcast.recv_buffer_nb(output,
                                                             ORTE_RMCAST_TAG_WILDCARD,
                                                             ORTE_RMCAST_PERSISTENT,
                                                             recv_input_buffers, NULL))) {
            ORTE_ERROR_LOG(rc);
            OPAL_THREAD_UNLOCK(&lock);
            return;
        }
        /* we only need to do this once - we'll sort out the
         * deliveries when we recv a message
         */
        break;
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement from source %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(&sender)));
    
    /* do we already know this source? */
    source = (orcm_pnp_source_t*)opal_pointer_array_get_item(&group->members, sender.vpid);
    if (NULL == source) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received adding source %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&sender)));
        
        source = OBJ_NEW(orcm_pnp_source_t);
        source->name.jobid = sender.jobid;
        source->name.vpid = sender.vpid;
        opal_pointer_array_set_item(&group->members, sender.vpid, source);
    }
    
    /* who are they responding to? */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &originator, &n, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        OPAL_THREAD_UNLOCK(&lock);
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
        OPAL_THREAD_UNLOCK(&lock);
        return;
    }
    
    /* if we get here, then this is an original announcement,
     * so we need to let them know we are here
     */
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement sending response",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* assemble the announcement response */
    OBJ_CONSTRUCT(&ann, opal_buffer_t);
    
    /* pack the common elements */
    if (ORCM_SUCCESS != (rc = pack_announcement(&ann))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&ann);
        OPAL_THREAD_UNLOCK(&lock);
        return;
    }
    /* tell everyone we are responding to an announcement
     * so they don't respond back
     */
    if (ORCM_SUCCESS != (rc = opal_dss.pack(&ann, &sender, 1, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        OBJ_DESTRUCT(&ann);
        OPAL_THREAD_UNLOCK(&lock);
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
    OPAL_THREAD_UNLOCK(&lock);
    return;
}

static void recv_inputs(int status,
                        orte_rmcast_channel_t channel,
                        orte_rmcast_tag_t tag,
                        orte_process_name_t *sender,
                        struct iovec *msg, int count, void *cbdata)
{
    orcm_pnp_group_t *group, *grp;
    orcm_pnp_source_t *src, *leader;
    int i, rc;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received input on channel %d from sender %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), channel,
                         ORTE_NAME_PRINT(sender)));
    
    /* since we are searching across global lists, lock
     * the thread
     */
    OPAL_THREAD_LOCK(&lock);
    
    /* the rmcast channel is associated with a group - get it */
    grp = NULL;
    for (i=0; i < groups.size; i++) {
        if (NULL == (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&groups, i))) {
            continue;
        }
        if (channel == group->channel) {
            grp = group;
            break;
        }
    }
    if (NULL == grp) {
        /* we don't know this channel yet - just ignore it */
        OPAL_THREAD_UNLOCK(&lock);
        return;
    }
    
    if (NULL == (src = (orcm_pnp_source_t*)opal_pointer_array_get_item(&grp->members, sender->vpid))) {
        /* don't know this sender - add it */
        src = OBJ_NEW(orcm_pnp_source_t);
        src->name.jobid = sender->jobid;
        src->name.vpid = sender->vpid;
        opal_pointer_array_set_item(&group->members, src->name.vpid, src);
    }
    
    /* add the message to the src */
    
    /* get the current leader for this group */
    leader = orcm_leader.get_leader(grp);
    
    /* if the leader is wildcard, then queue for delivery */
    if (NULL == leader) {
        ORCM_PROCESS_PNP_IOVECS(&recvs, &recvlock, &recvcond, grp, src,
                                channel, tag, msg, count, cbdata);
        goto DEPART;
    }
    
#if 0
    /* see if the leader has failed */
    if (orcm_leader.has_leader_failed(grp, leader)) {
        /* leader has failed - get new one */
        if (ORCM_SUCCESS != (rc = orcm_leader.select_leader(grp))) {
            ORTE_ERROR_LOG(rc);
        }
    }
    
    /* if this data came from the leader, queue for delivery */
    if (NULL != grp->leader && src == grp->leader) {
        ORCM_PROCESS_PNP_IOVECS(&recvs, &recvlock, &recvcond, grp, src,
                                channel, tag, msg, count, cbdata);
    }
#endif
    
DEPART:
#if 0
    /* update the msg number */
    src->last_msg_num = seq_num;
#endif
    /* clear the thread */
    OPAL_THREAD_UNLOCK(&lock);
}

static void recv_input_buffers(int status,
                               orte_rmcast_channel_t channel,
                               orte_rmcast_tag_t tag,
                               orte_process_name_t *sender,
                               opal_buffer_t *buf, void *cbdata)
{
    orcm_pnp_group_t *group, *grp;
    orcm_pnp_source_t *src, *leader;
    int i, rc;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received input buffer on channel %d from sender %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), channel,
                         ORTE_NAME_PRINT(sender)));
    
    /* since we are searching across global lists, lock
     * the thread
     */
    OPAL_THREAD_LOCK(&lock);
    
    /* the rmcast channel is associated with a group - get it */
    grp = NULL;
    for (i=0; i < groups.size; i++) {
        if (NULL == (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&groups, i))) {
            continue;
        }
        if (channel == group->channel) {
            grp = group;
            break;
        }
    }
    if (NULL == grp) {
        /* we don't know this channel yet - just ignore it */
        OPAL_THREAD_UNLOCK(&lock);
        return;
    }
    
    if (NULL == (src = (orcm_pnp_source_t*)opal_pointer_array_get_item(&grp->members, sender->vpid))) {
        /* don't know this sender - add it */
        src = OBJ_NEW(orcm_pnp_source_t);
        src->name.jobid = sender->jobid;
        src->name.vpid = sender->vpid;
        opal_pointer_array_set_item(&group->members, src->name.vpid, src);
    }
    
    /* add the message to the src */
    
    /* get the current leader for this group */
    leader = orcm_leader.get_leader(grp);

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received input buffer found sender %s in grp",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));
    
    /* if the leader is wildcard, then queue for delivery */
    if (NULL == leader) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received input buffer wildcard leader - delivering msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        
        ORCM_PROCESS_PNP_BUFFERS(&recvs, &recvlock, &recvcond, grp, src,
                                 channel, tag, buf, cbdata);
        goto DEPART;
    }
    
#if 0
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
    
    /* if this data came from the leader, queue it for delivery */
    if (NULL != grp->leader && src == grp->leader) {
        ORCM_PROCESS_PNP_BUFFERS(&recvs, &recvlock, &recvcond, grp, src,
                                 channel, tag, buf, cbdata);
    }
#endif
    
DEPART:
#if 0
    /* update the msg number */
    src->last_msg_num = seq_num;
#endif
    /* clear the thread */
    OPAL_THREAD_UNLOCK(&lock);
}

/* pack the common elements of an announcement message */
static int pack_announcement(opal_buffer_t *buf)
{
    int ret;
    
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, ORTE_PROC_MY_NAME, 1, ORTE_NAME))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
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
    /* pack my output channel */
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
                                   opal_buffer_t *buf, void* cbdata)
{
    orcm_pnp_send_t *send = (orcm_pnp_send_t*)cbdata;
    
    if (NULL != send->cbfunc_buf) {
        send->cbfunc_buf(status, ORTE_PROC_MY_NAME, send->tag, send->buffer, send->cbdata);
    }
    OBJ_RELEASE(send);
}

static void rmcast_callback(int status,
                            orte_rmcast_channel_t channel,
                            orte_rmcast_tag_t tag,
                            orte_process_name_t *sender,
                            struct iovec *msg, int count, void* cbdata)
{
    orcm_pnp_send_t *send = (orcm_pnp_send_t*)cbdata;
    
    if (NULL != send->cbfunc) {
        send->cbfunc(status, ORTE_PROC_MY_NAME, send->tag, send->msg, send->count, send->cbdata);
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
    
    /* release the scratch buffer */
    OBJ_RELEASE(buffer);
    /* do any required callbacks */
    if (NULL != send->cbfunc_buf) {
        send->cbfunc_buf(status, ORTE_PROC_MY_NAME, send->tag, send->buffer, send->cbdata);
    }
    OBJ_RELEASE(send);
}


/* A message packet from a NULL group signals
 * that the thread should terminate
 */
static void* recv_messages(opal_object_t *obj)
{
    orcm_pnp_recv_t *msgpkt;
    orte_process_name_t sender;
    orcm_pnp_group_t *group;
    orcm_pnp_pending_request_t *request;
    int i, ret;
    
    while (1) {
        OPAL_THREAD_LOCK(&recvlock);
        
        while (0 == opal_list_get_size(&recvs)) {
            opal_condition_wait(&recvcond, &recvlock);
        }
        
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default processing recv list",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        
        /* process all messages on the list */
        while (NULL != (msgpkt = (orcm_pnp_recv_t*)opal_list_remove_first(&recvs))) {
            
            /* see if this is the terminator message */
            if (NULL == msgpkt->grp) {
                /* time to exit! */
                OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                     "%s pnp:default recv terminate msg recvd",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
                return OPAL_THREAD_CANCELLED;
            }
            
            sender.jobid = msgpkt->src->name.jobid;
            sender.vpid = msgpkt->src->name.vpid;
            group = msgpkt->grp;
            
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s pnp:default receiving msg from %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                 ORTE_NAME_PRINT(&sender)));
            
            /* extract the request object for this tag */
            if (NULL == (request = (orcm_pnp_pending_request_t*)opal_pointer_array_get_item(&group->requests, msgpkt->tag))) {
                /* if there isn't one for this specific tag, is there one for the wildcard tag? */
                if (NULL == (request = (orcm_pnp_pending_request_t*)opal_pointer_array_get_item(&group->requests, ORCM_PNP_TAG_WILDCARD))) {
                    /* no available requests - just move on */
                    OBJ_RELEASE(msgpkt);
                    continue;
                }
            }
            /* found it! deliver the msg */
            if (NULL != msgpkt->buffer && NULL != request->cbfunc_buf) {
                request->cbfunc_buf(ORCM_SUCCESS, &sender, msgpkt->tag, msgpkt->buffer, NULL);
            } else if (NULL != msgpkt->msg && NULL != request->cbfunc) {
                request->cbfunc(ORCM_SUCCESS, &sender, msgpkt->tag, msgpkt->msg, msgpkt->count, NULL);
            }
            OBJ_RELEASE(msgpkt);
        }
        /* release the lock */
        OPAL_THREAD_UNLOCK(&recvlock);
    }
}

static void recv_direct_msgs(int status, orte_process_name_t* sender,
                             opal_buffer_t* buffer, orte_rml_tag_t tg,
                             void* cbdata)
{
    int rc;
    int32_t n, i, sz, iovec_count;
    int8_t flag;
    orcm_pnp_tag_t tag;
    struct iovec *iovec_array;
    orcm_pnp_group_t *group, *grp;
    orcm_pnp_source_t *source, *src;
    opal_list_item_t *item;
    orte_rmcast_channel_t channel;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default recvd direct msg from %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));
    
    /* since we are searching across global lists, lock
     * the thread
     */
    OPAL_THREAD_LOCK(&lock);
    
    /* unpack the channel */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buffer, &channel, &n, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    
    /* locate this sender on our list */
    grp = NULL;
    for (i=0; i < groups.size; i++) {
        if (NULL == (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&groups, i))) {
            continue;
        }
        if (channel == group->channel) {
            grp = group;
            break;
        }
    }
    if (NULL == grp) {
        /* we don't know this channel yet - just ignore it */
        goto CLEANUP;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received input buffer found grp",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    if (NULL == (src = (orcm_pnp_source_t*)opal_pointer_array_get_item(&grp->members, sender->vpid))) {
        /* don't know this sender - add it */
        src = OBJ_NEW(orcm_pnp_source_t);
        src->name.jobid = sender->jobid;
        src->name.vpid = sender->vpid;
        opal_pointer_array_set_item(&group->members, src->name.vpid, src);
    }
    
    /* unpack the flag */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buffer, &flag, &n, OPAL_INT8))) {
        goto CLEANUP;
    }
    
    /* unpack the intended tag for this message */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buffer, &tag, &n, ORCM_PNP_TAG_T))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    
    if (1 == flag) {
        /* buffer was included */
        ORCM_PROCESS_PNP_BUFFERS(&recvs, &recvlock, &recvcond, grp, src,
                                 channel, tag, buffer, NULL);
    } else {
        /* iovecs included - get the number of iovecs in the buffer */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &iovec_count, &n, OPAL_INT32))) {
            ORTE_ERROR_LOG(rc);
            goto CLEANUP;
        }
        /* malloc the required space */
        iovec_array = (struct iovec *)malloc(iovec_count * sizeof(struct iovec));
        /* unpack the iovecs */
        for (i=0; i < iovec_count; i++) {
            /* unpack the number of bytes in this iovec */
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &sz, &n, OPAL_INT32))) {
                ORTE_ERROR_LOG(rc);
                goto CLEANUP;
            }
            /* allocate the space */
            iovec_array[i].iov_base = (uint8_t*)malloc(sz);
            iovec_array[i].iov_len = sz;
            /* unpack the data */
            n=sz;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, iovec_array[i].iov_base, &n, OPAL_UINT8))) {
                ORTE_ERROR_LOG(rc);
                goto CLEANUP;
            }                    
        }
        /* queue the message */
        ORCM_PROCESS_PNP_IOVECS(&recvs, &recvlock, &recvcond, grp, src,
                                channel, tag, iovec_array, iovec_count, NULL);
    }
    
CLEANUP:
    /* reissue the recv */
    if (ORTE_SUCCESS != (rc = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD,
                                                      ORTE_RML_TAG_MULTICAST_DIRECT,
                                                      ORTE_RML_NON_PERSISTENT,
                                                      recv_direct_msgs,
                                                      NULL))) {
        ORTE_ERROR_LOG(rc);
    }
    
    /* unlock the thread */
    OPAL_THREAD_UNLOCK(&lock);
}

static orcm_pnp_channel_tracker_t* get_channel(char *app,
                                               char *version,
                                               char *release)
{
    int i, j;
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *group;
    orcm_pnp_pending_request_t *request;
    
    for (i=0; i < channels.size; i++) {
        if (NULL == (tracker = (orcm_pnp_channel_tracker_t*)opal_pointer_array_get_item(&channels, i))) {
            continue;
        }
        if (0 != strcasecmp(app, tracker->app)) {
            continue;
        }
        if ((NULL == version && NULL != tracker->version) ||
            (NULL != version && NULL == tracker->version)) {
            continue;
        }
        if (NULL != version && 0 != strcasecmp(version, tracker->version)) {
            continue;
        }
        if ((NULL == release && NULL != tracker->release) ||
            (NULL != release && NULL == tracker->release)) {
            continue;
        }
        if (NULL != release && 0 != strcasecmp(release, tracker->release)) {
            continue;
        }
        /* if we get here, then we have a match */
        return tracker;
    }
    
    /* if we get here, then this triplet doesn't exist - create it */
    tracker = OBJ_NEW(orcm_pnp_channel_tracker_t);
    tracker->app = strdup(app);
    if (NULL != version) {
        tracker->version = strdup(version);
    }
    if (NULL != release) {
        tracker->release = strdup(release);
    }
    tracker->channel = my_pnp_channels++;
    opal_pointer_array_add(&channels, tracker);
    
    /* check all known groups to see who might belong to this new channel */
    for (i=0; i < groups.size; i++) {
        if (NULL == (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&groups, i))) {
            continue;
        }
        if (0 != strcasecmp(app, group->app)) {
            continue;
        }
        if (NULL != version && 0 != strcasecmp(group->version, version)) {
            continue;
        }
        if (NULL != release && 0 != strcasecmp(group->release, release)) {
            continue;
        }
        /* have a match - add the group */
        OBJ_RETAIN(group);
        opal_pointer_array_add(&tracker->groups, group);
        /* add any pending requests associated with this channel */
        for (j=0; j < group->requests.size; j++) {
            if (NULL == (request = (orcm_pnp_pending_request_t*)opal_pointer_array_get_item(&group->requests, j))) {
                continue;
            }
            if (NULL != opal_pointer_array_get_item(&tracker->requests, request->tag)) {
                /* already assigned */
                continue;
            }
            OBJ_RETAIN(request);
            opal_pointer_array_set_item(&tracker->requests, request->tag, request);
        }
    }
    
    return tracker;
}

static orcm_pnp_channel_tracker_t* find_channel(orcm_pnp_channel_t channel)
{
    orcm_pnp_channel_tracker_t *tracker;
    int i, j, ret;
    
    /* bozo check */
    if (ORCM_PNP_INVALID_CHANNEL == channel) {
        /* throw bits on floor */
        return NULL;
    }
    
    for (i=0; i < channels.size; i++) {
        if (NULL == (tracker = (orcm_pnp_channel_tracker_t*)opal_pointer_array_get_item(&channels, i))) {
            continue;
        }
        if (channel == tracker->channel) {
            /* found it! */
            return tracker;
        }
    }
    
    /* get here if we don't have it */
    return NULL;
}

static void setup_recv_request(orcm_pnp_channel_tracker_t *tracker,
                               orcm_pnp_tag_t tag,
                               orcm_pnp_callback_fn_t cbfunc,
                               orcm_pnp_callback_buffer_fn_t cbfunc_buf)
{
    orcm_pnp_pending_request_t *request;
    orcm_pnp_group_t *group;
    int i;
    
    if (NULL == (request = (orcm_pnp_pending_request_t*)opal_pointer_array_get_item(&tracker->requests, tag))) {
        request = OBJ_NEW(orcm_pnp_pending_request_t);
        request->tag = tag;
        opal_pointer_array_set_item(&tracker->requests, request->tag, request);
    }
    
    /* setup the callback functions */
    if (NULL == request->cbfunc) {
        request->cbfunc = cbfunc;
    }
    if (NULL == request->cbfunc_buf) {
        request->cbfunc_buf = cbfunc_buf;
    }
    
    /* indicate these groups are active */
    for (i=0; i < tracker->groups.size; i++) {
        if (NULL == (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
            continue;
        }
        if (NULL != opal_pointer_array_get_item(&group->requests, request->tag)) {
            /* already assigned */
            continue;
        }
        OBJ_RETAIN(request);
        opal_pointer_array_set_item(&group->requests, request->tag, request);
    }
}

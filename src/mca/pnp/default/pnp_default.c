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
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>

#include "opal/dss/dss.h"
#include "opal/class/opal_list.h"
#include "opal/class/opal_pointer_array.h"
#include "opal/util/output.h"
#include "opal/threads/threads.h"
#include "opal/mca/sysinfo/sysinfo.h"

#include "orte/mca/rmcast/rmcast.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rml/rml.h"
#include "orte/runtime/orte_globals.h"

#include "runtime/runtime.h"

#include "mca/pnp/pnp.h"
#include "mca/pnp/base/private.h"
#include "mca/pnp/default/pnp_default.h"

/* API functions */

static int default_init(void);
static int announce(char *app, char *version, char *release,
                    orcm_pnp_announce_fn_t cbfunc);
static orcm_pnp_channel_t open_channel(char *app, char *version, char *release,
                                       orcm_pnp_open_channel_cbfunc_t cbfunc);
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
                               orte_rmcast_channel_t channel,
                               orte_rmcast_tag_t tag,
                               orte_process_name_t *sender,
                               opal_buffer_t *buf, void *cbdata);
static void recv_input_buffers(int status,
                               orte_rmcast_channel_t channel,
                               orte_rmcast_tag_t tag,
                               orte_process_name_t *sender,
                               opal_buffer_t *buf, void *cbdata);
static int pack_announcement(opal_buffer_t *buf, orte_process_name_t *name);

static void rmcast_callback_buffer(int status,
                                   orte_rmcast_channel_t channel,
                                   orte_rmcast_tag_t tag,
                                   orte_process_name_t *sender,
                                   opal_buffer_t *buf, void* cbdata);

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

static orcm_pnp_triplet_t* get_triplet(char *app,
                                       char *version,
                                       char *release);

static void setup_recv_request(orcm_pnp_triplet_t *triplet,
                               orcm_pnp_tag_t tag,
                               orcm_pnp_callback_fn_t cbfunc,
                               orcm_pnp_callback_buffer_fn_t cbfunc_buf);

static orcm_pnp_request_t* find_request(opal_list_t *list,
                                        orcm_pnp_tag_t tag);

static int construct_msg(opal_buffer_t **buf, opal_buffer_t *buffer,
                         orcm_pnp_tag_t tag, struct iovec *msg, int count);

static bool triplet_cmp(char *str1, char *str2);

/* Local variables */
static opal_pointer_array_t triplets;
static int32_t packet_number = 0;
static bool recv_on = false;
static char *my_string_id = NULL;
static uint32_t my_uid;
static orcm_pnp_announce_fn_t my_announce_cbfunc = NULL;
static orte_rmcast_channel_t my_channel;
static orcm_pnp_triplet_t *my_triplet=NULL;

/* local thread support */
static opal_mutex_t lock;
static opal_condition_t cond;
static bool active = false;

static int default_init(void)
{
    int ret;
    orcm_pnp_triplet_t *triplet;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "pnp default init"));
    
    /* sanity check in case ORTE was built without --enable-multicast */
    if (NULL == orte_rmcast.open_channel) {
        orte_show_help("help-pnp-default.txt", "multicast-disabled", true);
        return ORCM_ERR_NOT_SUPPORTED;
    }
    
    /* init the array of triplets */
    OBJ_CONSTRUCT(&triplets, opal_pointer_array_t);
    opal_pointer_array_init(&triplets, 8, INT_MAX, 8);
    
    /* setup the threading support */
    OBJ_CONSTRUCT(&lock, opal_mutex_t);
    OBJ_CONSTRUCT(&cond, opal_condition_t);
    
    /* record my channel */
    my_channel = orte_rmcast.query_channel();
    
    /* record my uid */
    my_uid = (uint32_t)getuid();
    
    /* setup a recv to catch any announcements */
    if (!recv_on) {
        /* setup the respective public address channel */
        if (ORCM_PROC_IS_MASTER || ORCM_PROC_IS_DAEMON || ORCM_PROC_IS_TOOL) {
            /* setup a triplet */
            triplet = OBJ_NEW(orcm_pnp_triplet_t);
            ORCM_PNP_CREATE_STRING_ID(&triplet->string_id,"ORCM_SYSTEM",NULL,NULL);
            triplet->channel = ORTE_RMCAST_SYS_CHANNEL;
            /* add to the list of groups */
            opal_pointer_array_add(&triplets, triplet);
            /* open a channel to this group - will just return if
             * the channel already exists
             */
            if (ORCM_SUCCESS != (ret = orte_rmcast.open_channel(triplet->channel,
                                                                triplet->string_id,
                                                                NULL, -1, NULL,
                                                                ORTE_RMCAST_BIDIR))) {
                if (ORTE_EXISTS != ret) {
                    ORTE_ERROR_LOG(ret);
                    return ret;
                }
            }
            /* setup to listen to it - will just return if we already are */
            if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(triplet->channel,
                                                                  ORTE_RMCAST_TAG_ANNOUNCE,
                                                                  ORTE_RMCAST_PERSISTENT,
                                                                  recv_announcements, NULL))) {
                if (ORTE_EXISTS != ret) {
                    ORTE_ERROR_LOG(ret);
                    return ret;
                }
            }
        } else if (ORCM_PROC_IS_APP) {
            /* setup a triplet */
            triplet = OBJ_NEW(orcm_pnp_triplet_t);
            ORCM_PNP_CREATE_STRING_ID(&triplet->string_id,"ORCM_APP_ANNOUNCE",NULL,NULL);
            triplet->channel = ORTE_RMCAST_APP_PUBLIC_CHANNEL;
            /* add to the list of triplets */
            opal_pointer_array_add(&triplets, triplet);
            
            /* open the channel */
            if (ORCM_SUCCESS != (ret = orte_rmcast.open_channel(triplet->channel,
                                                                triplet->string_id,
                                                                NULL, -1, NULL,
                                                                ORTE_RMCAST_BIDIR))) {
                if (ORTE_EXISTS != ret) {
                    ORTE_ERROR_LOG(ret);
                    return ret;
                }
            }
            /* setup to listen to it - will just return if we already are */
            if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(triplet->channel,
                                                                  ORTE_RMCAST_TAG_ANNOUNCE,
                                                                  ORTE_RMCAST_PERSISTENT,
                                                                  recv_announcements, NULL))) {
                if (ORTE_EXISTS != ret) {
                    ORTE_ERROR_LOG(ret);
                    return ret;
                }
            }
        }
        
        /* setup an RML recv to catch any direct messages */
        if (ORTE_SUCCESS != (ret = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD,
                                                           ORTE_RML_TAG_MULTICAST_DIRECT,
                                                           ORTE_RML_NON_PERSISTENT,
                                                           recv_direct_msgs,
                                                           NULL))) {
            if (ORTE_EXISTS != ret) {
                ORTE_ERROR_LOG(ret);
                return ret;
            }
        }
        recv_on = true;
    }
    
    return ORCM_SUCCESS;
}

static int announce(char *app, char *version, char *release,
                    orcm_pnp_announce_fn_t cbfunc)
{
    int ret;
    opal_buffer_t buf;
    orcm_pnp_triplet_t *triplet;
    orcm_pnp_channel_t chan;
    opal_list_item_t *itm2;
    orcm_pnp_request_t *req, *request;
    
    /* bozo check */
    if (NULL == app || NULL == version || NULL == release) {
        ORTE_ERROR_LOG(ORCM_ERR_BAD_PARAM);
        return ORCM_ERR_BAD_PARAM;
    }
    
    /* protect against threading */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    if (NULL != my_string_id) {
        /* must have been called before */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:announce called before",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORCM_SUCCESS;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:announce app %s version %s release %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         app, version, release));
    
    /* retain a local record of my info */
    ORCM_PNP_CREATE_STRING_ID(&my_string_id, app, version, release);
    
    /* retain the callback function */
    my_announce_cbfunc = cbfunc;
    
    /* get a triplet object for this triplet - creates
     * it if one doesn't already exist
     */
    triplet = get_triplet(app, version, release);
    triplet->channel = my_channel;
    my_triplet = triplet;

    /* do we want to listen to our input? */
    if (0 < opal_list_get_size(&triplet->requests)) {
        /* open the channel */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:ann setup recv for channel %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), my_channel));
        /* setup the recv */
        if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(my_channel,
                                                             ORTE_RMCAST_TAG_WILDCARD,
                                                             ORTE_RMCAST_PERSISTENT,
                                                             recv_input_buffers, NULL))) {
            ORTE_ERROR_LOG(ret);
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            return ret;
        }
    }

    /* no need to hold the lock any further */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
    /* assemble the announcement message */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    
    /* pack the common elements */
    if (ORCM_SUCCESS != (ret = pack_announcement(&buf, ORTE_NAME_INVALID))) {
        ORTE_ERROR_LOG(ret);
        OBJ_DESTRUCT(&buf);
        return ret;
    }
    
    /* select the channel */
    if (ORCM_PROC_IS_APP) {
        chan = ORTE_RMCAST_APP_PUBLIC_CHANNEL;
    } else {
        chan = ORTE_RMCAST_SYS_CHANNEL;
    }
    
    /* send it */
    if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer(chan, ORTE_RMCAST_TAG_ANNOUNCE, &buf))) {
        ORTE_ERROR_LOG(ret);
    }
    
    /* cleanup */
    OBJ_DESTRUCT(&buf);
    
    return ret;
}

static int open_channel(char *app, char *version, char *release,
                        orcm_pnp_open_channel_cbfunc_t cbfunc)
{
    orcm_pnp_triplet_t *triplet;
    orcm_pnp_request_t *request;
    opal_list_item_t *item;
    int i, rc;
    
    /* protect the global arrays */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* see if we already know this triplet - automatically
     * creates it if not
     */
    triplet = get_triplet(app, version, release);
    triplet->cbfunc = cbfunc;
    
    /* if we know this triplet's channel */
    if (ORCM_PNP_INVALID_CHANNEL != triplet->channel) {
        cbfunc(app, version, release, triplet->channel);
        triplet->cbfunc = NULL;
    }
    
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
    return ORCM_SUCCESS;
}

static int register_input(char *app,
                          char *version,
                          char *release,
                          orcm_pnp_tag_t tag,
                          orcm_pnp_callback_fn_t cbfunc)
{
    orcm_pnp_triplet_t *triplet;
    int ret=ORCM_SUCCESS;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:register_input app %s version %s release %s tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release, tag));
    
    /* since we are modifying global lists, lock the thread */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* get a triplet object for this triplet - creates
     * it if one doesn't already exist
     */
    triplet = get_triplet(app, version, release);  
    
    /* record the request - will just return if this request
     * already exists
     */
    setup_recv_request(triplet, tag, cbfunc, NULL);
    
    if (ORCM_PNP_INVALID_CHANNEL == triplet->channel) {
        /* can't open it yet since we don't know the channel */
        goto cleanup;
    }
    
    /* open this triplet's channel - will just return if already open */
    if (ORCM_SUCCESS != (ret = orte_rmcast.open_channel(triplet->channel,
                                                        triplet->string_id,
                                                        NULL, -1, NULL,
                                                        ORTE_RMCAST_BIDIR))) {
        if (ORTE_EXISTS != ret) {
            ORTE_ERROR_LOG(ret);
            goto cleanup;
        }
    }
    /* setup to listen to it - will just return if we already are */
    if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(triplet->channel,
                                                          ORTE_RMCAST_TAG_WILDCARD,
                                                          ORTE_RMCAST_PERSISTENT,
                                                          recv_input_buffers, NULL))) {
        if (ORTE_EXISTS == ret) {
            ret = ORTE_SUCCESS;
            goto cleanup;
        }
        ORTE_ERROR_LOG(ret);
    }
    
cleanup:
    /* clear the thread */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
    return ret;
}

static int register_input_buffer(char *app,
                                 char *version,
                                 char *release,
                                 orcm_pnp_tag_t tag,
                                 orcm_pnp_callback_buffer_fn_t cbfunc)
{
    orcm_pnp_triplet_t *triplet;
    int i, ret=ORCM_SUCCESS;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:register_input_buffer app %s version %s release %s tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release, tag));
    
    /* since we are modifying global lists, lock
     * the thread
     */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* get a triplet object for this triplet - creates
     * it if one doesn't already exist
     */
    triplet = get_triplet(app, version, release);        
    
    /* record the request - will just return if this request
     * already exists
     */
    setup_recv_request(triplet, tag, NULL, cbfunc);
    
    if (ORCM_PNP_INVALID_CHANNEL == triplet->channel) {
        /* can't open it yet since we don't know the channel */
        goto cleanup;
    }
    
    /* open this triplet's channel - will just return if already open */
    if (ORCM_SUCCESS != (ret = orte_rmcast.open_channel(triplet->channel,
                                                        triplet->string_id,
                                                        NULL, -1, NULL,
                                                        ORTE_RMCAST_BIDIR))) {
        if (ORTE_EXISTS != ret) {
            ORTE_ERROR_LOG(ret);
            goto cleanup;
        }
    }
    /* setup to listen to it - will just return if we already are */
    if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(triplet->channel,
                                                          ORTE_RMCAST_TAG_WILDCARD,
                                                          ORTE_RMCAST_PERSISTENT,
                                                          recv_input_buffers, NULL))) {
        if (ORTE_EXISTS == ret) {
            ret = ORTE_SUCCESS;
            goto cleanup;
        }
        ORTE_ERROR_LOG(ret);
    }
    
cleanup:
    /* clear the thread */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
    return ret;
}

static int deregister_input(char *app,
                            char *version,
                            char *release,
                            orcm_pnp_tag_t tag)
{
    orcm_pnp_triplet_t *triplet;
    orcm_pnp_request_t *request;
    opal_list_item_t *item;
    int ret=ORCM_SUCCESS;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:deregister_input app %s version %s release %s tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release, tag));
    
    /* since we are modifying global lists, lock
     * the thread
     */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* get a triplet object for this triplet - creates
     * it if one doesn't already exist
     */
    triplet = get_triplet(app, version, release);        
    
    /* if this is a wildcard tag, remove all recvs */
    if (ORCM_PNP_TAG_WILDCARD == tag) {
        while (NULL != (item = opal_list_remove_first(&triplet->requests))) {
            OBJ_RELEASE(item);
        }
    } else {
        /* remove the recv for this tag, if it exists */
        if (NULL != (request = find_request(&triplet->requests, tag))) {
            opal_list_remove_item(&triplet->requests, &request->super);
            OBJ_RELEASE(request);
        }
    }
    
cleanup:
    /* clear the thread */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
    return ret;
}

static int default_output(orcm_pnp_channel_t channel,
                          orte_process_name_t *recipient,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg, int count)
{
    int i, ret;
    opal_buffer_t *buf;
    orcm_pnp_channel_t chan;
    
    /* protect against threading */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* setup the message for xmission */
    if (ORTE_SUCCESS != (ret = construct_msg(&buf, NULL, tag, msg, count))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }
    
    /* if this is intended for everyone who might be listening on this channel,
     * multicast it to all groups in this channel
     */
    if (NULL == recipient ||
        (ORTE_JOBID_WILDCARD == recipient->jobid &&
         ORTE_VPID_WILDCARD == recipient->vpid)) {
        /* if this is going on the group channel, then substitute that channel here */
        if (ORCM_PNP_GROUP_CHANNEL == channel) {
            chan = my_channel;
        } else {
            chan = channel;
        }
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:sending multicast of %d iovecs to channel %d tag %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), count, chan, tag));
        
        /* send the iovecs to the channel */
        if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer(chan, tag, buf))) {
            ORTE_ERROR_LOG(ret);
        }
        OBJ_RELEASE(buf);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORCM_SUCCESS;
    }
    
    /* if only one name field is WILDCARD, I don't know how to send
     * it - at least, not right now
     */
    if (ORTE_JOBID_WILDCARD == recipient->jobid ||
        ORTE_VPID_WILDCARD == recipient->vpid) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_IMPLEMENTED);
        OBJ_RELEASE(buf);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORTE_ERR_NOT_IMPLEMENTED;
    }
    
    /* intended for a specific recipient, send it over p2p */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending p2p message of %d iovecs to tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), count, tag));

    /* release the thread prior to send */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
    /* send the msg */
    if (0 > (ret = orte_rml.send_buffer(recipient, buf, ORTE_RML_TAG_MULTICAST_DIRECT, 0))) {
        ORTE_ERROR_LOG(ret);
    } else {
        ret = ORCM_SUCCESS;
    }
    OBJ_RELEASE(buf);
    return ret;
}

static int default_output_nb(orcm_pnp_channel_t channel,
                             orte_process_name_t *recipient,
                             orcm_pnp_tag_t tag,
                             struct iovec *msg, int count,
                             orcm_pnp_callback_fn_t cbfunc,
                             void *cbdata)
{
    int i, ret;
    orcm_pnp_send_t *send;
    opal_buffer_t *buf;
    orcm_pnp_channel_t chan;

    /* protect against threading */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    send = OBJ_NEW(orcm_pnp_send_t);
    send->tag = tag;
    send->msg = msg;
    send->count = count;
    send->cbfunc = cbfunc;
    send->cbdata = cbdata;

    /* setup the message for xmission */
    if (ORTE_SUCCESS != (ret = construct_msg(&buf, NULL, tag, msg, count))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }
    send->buffer = buf;
    
    /* if this is intended for everyone who might be listening to my output,
     * multicast it
     */
    if (NULL == recipient ||
        (ORTE_JOBID_WILDCARD == recipient->jobid &&
         ORTE_VPID_WILDCARD == recipient->vpid)) {
        /* if this is going on the group channel, then substitute that channel here */
        if (ORCM_PNP_GROUP_CHANNEL == channel) {
            chan = my_channel;
        } else {
            chan = channel;
        }
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:sending multicast of %d iovecs to tag %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), count, tag));
        
        /* release thread prior to send */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        /* send the iovecs to the channel */
        if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer_nb(chan, tag, buf,
                                                              rmcast_callback_buffer, send))) {
            ORTE_ERROR_LOG(ret);
        }
        return ORCM_SUCCESS;
    }
    
    /* if only one name field is WILDCARD, I don't know how to send
     * it - at least, not right now
     */
    if (ORTE_JOBID_WILDCARD == recipient->jobid ||
        ORTE_VPID_WILDCARD == recipient->vpid) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_IMPLEMENTED);
        OBJ_RELEASE(send);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORTE_ERR_NOT_IMPLEMENTED;
    }
    
    /* intended for a specific recipient, send it over p2p */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending p2p message of %d iovecs to tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), count, tag));
    /* release thread prior to send */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
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
    int i, ret;
    opal_buffer_t *buf;
    orcm_pnp_channel_t chan;
    
    /* protect against threading */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* setup the message for xmission */
    if (ORTE_SUCCESS != (ret = construct_msg(&buf, buffer, tag, NULL, 0))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }
    
    /* if this is intended for everyone who might be listening on this channel,
     * multicast it
     */
    if (NULL == recipient ||
        (ORTE_JOBID_WILDCARD == recipient->jobid &&
         ORTE_VPID_WILDCARD == recipient->vpid)) {
        /* if this is going on the group channel, then substitute that channel here */
        if (ORCM_PNP_GROUP_CHANNEL == channel) {
            chan = my_channel;
        } else {
            chan = channel;
        }
        
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending multicast buffer of %d bytes to channel %d tag %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)buf->bytes_used,
                             chan, tag));
        /* release thread prior to send */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer(chan, tag, buf))) {
            ORTE_ERROR_LOG(ret);
        }
        OBJ_RELEASE(buf);
        return ORCM_SUCCESS;
    }
    
    /* if only one name field is WILDCARD, I don't know how to send
     * it - at least, not right now
     */
    if (ORTE_JOBID_WILDCARD == recipient->jobid ||
        ORTE_VPID_WILDCARD == recipient->vpid) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_IMPLEMENTED);
        OBJ_RELEASE(buf);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORCM_ERR_NOT_IMPLEMENTED;
    }
    
    /* intended for a specific recipient, send it over p2p */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending p2p buffer of %d bytes to proc %s tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (int)buffer->bytes_used,
                         ORTE_NAME_PRINT(recipient), tag));
    
    /* release thread prior to send */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
    if (0 > (ret = orte_rml.send_buffer(recipient, buf, ORTE_RML_TAG_MULTICAST_DIRECT, 0))) {
        ORTE_ERROR_LOG(ret);
    } else {
        ret = ORCM_SUCCESS;
    }
    OBJ_RELEASE(buf);
    return ret;
}

static int default_output_buffer_nb(orcm_pnp_channel_t channel,
                                    orte_process_name_t *recipient,
                                    orcm_pnp_tag_t tag,
                                    opal_buffer_t *buffer,
                                    orcm_pnp_callback_buffer_fn_t cbfunc,
                                    void *cbdata)
{
    int i, ret;
    orcm_pnp_send_t *send;
    opal_buffer_t *buf;
    int8_t flag;
    orcm_pnp_channel_t chan;
    
    /* protect against threading */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    send = OBJ_NEW(orcm_pnp_send_t);
    send->tag = tag;
    send->cbfunc_buf = cbfunc;
    send->cbdata = cbdata;
    
    /* setup the message for xmission */
    if (ORTE_SUCCESS != (ret = construct_msg(&buf, buffer, tag, NULL, 0))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }
    send->buffer = buf;
    
    /* if this is intended for everyone who might be listening to my output,
     * multicast it
     */
    if (NULL == recipient ||
        (ORTE_JOBID_WILDCARD == recipient->jobid &&
         ORTE_VPID_WILDCARD == recipient->vpid)) {
        /* if this is going on the group channel, then substitute that channel here */
        if (ORCM_PNP_GROUP_CHANNEL == channel) {
            chan = my_channel;
        } else {
            chan = channel;
        }
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:sending multicast buffer of %d bytes to channel %d tag %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)buffer->bytes_used, chan, tag));
        
        /* release thread prior to send */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        /* send the buffer to the channel */
        if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer_nb(chan, tag, buf,
                                                              rmcast_callback_buffer, send))) {
            ORTE_ERROR_LOG(ret);
        }
        return ORCM_SUCCESS;
    }
    
    /* if only one name field is WILDCARD, I don't know how to send
     * it - at least, not right now
     */
    if (ORTE_JOBID_WILDCARD == recipient->jobid ||
        ORTE_VPID_WILDCARD == recipient->vpid) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_IMPLEMENTED);
        OBJ_RELEASE(send);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORTE_ERR_NOT_IMPLEMENTED;
    }
    
    /* intended for a specific recipient, send it over p2p */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending p2p buffer of %d bytes to tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)buffer->bytes_used, tag));
    
    /* release thread prior to send */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
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
    orcm_pnp_triplet_t *triplet;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default: finalizing",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* cancel the recvs, if active */
    if (recv_on) {
        orte_rmcast.cancel_recv(my_triplet->channel, ORTE_RMCAST_TAG_WILDCARD);
        orte_rml.recv_cancel(ORTE_NAME_WILDCARD, ORTE_RML_TAG_MULTICAST_DIRECT);
        recv_on = false;
    }
    
    /* destruct the threading support */
    OBJ_DESTRUCT(&lock);
    OBJ_DESTRUCT(&cond);
    
    /* release the array of known triplets */
    for (i=0; i < triplets.size; i++) {
        if (NULL != (triplet = (orcm_pnp_triplet_t*)opal_pointer_array_get_item(&triplets, i))) {
            OBJ_RELEASE(triplet);
        }
    }
    OBJ_DESTRUCT(&triplets);
    
    return ORCM_SUCCESS;
}


/****    LOCAL  FUNCTIONS    ****/
static void recv_announcements(int status,
                               orte_rmcast_channel_t channel,
                               orte_rmcast_tag_t tag,
                               orte_process_name_t *sndr,
                               opal_buffer_t *buf, void *cbdata)
{
    opal_list_item_t *itm2;
    orcm_pnp_triplet_t *triplet;
    orcm_pnp_source_t *source;
    char *app=NULL, *version=NULL, *release=NULL, *string_id=NULL, *nodename=NULL;
    orte_process_name_t originator;
    opal_buffer_t ann;
    int rc, n, i, j;
    orcm_pnp_request_t *request, *req;
    orte_rmcast_channel_t output;
    orte_process_name_t sender;
    orcm_pnp_send_t *pkt;
    orcm_pnp_channel_t chan;
    orte_job_t *daemons;
    orte_proc_t *proc;
    uint32_t uid;
    bool known=true;
    char *rml_uri=NULL;
    
    /* unpack the name of the sender */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &sender, &n, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* unpack the app's triplet */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &string_id, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    /* get its multicast channel */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &output, &n, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* get its nodename */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &nodename, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* get its uid */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &uid, &n, OPAL_UINT32))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* unpack the its rml uri */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &rml_uri, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement from app %s channel %d on node %s uid %u",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         string_id, output, nodename, uid));
    
    /* since we are accessing global lists, acquire the thread */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* do we already know this triplet? */
    for (i=0; i < triplets.size; i++) {
        if (NULL == (triplet = (orcm_pnp_triplet_t*)opal_pointer_array_get_item(&triplets, i))) {
            continue;
        }
        
        /* the triplet must be unique */
        if (triplet_cmp(triplet->string_id, string_id)) {
            /* record the multicast channel it is on */
            triplet->channel = output;
            /* do we want to listen to this triplet? */
            if (0 < opal_list_get_size(&triplet->requests)) {
                /* open the channel */
                OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                     "%s pnp:default:recvd_ann setup recv for channel %d",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), output));
                /* setup the recv */
                if (ORTE_SUCCESS != (rc = orte_rmcast.recv_buffer_nb(output,
                                                                     ORTE_RMCAST_TAG_WILDCARD,
                                                                     ORTE_RMCAST_PERSISTENT,
                                                                     recv_input_buffers, NULL))) {
                    ORTE_ERROR_LOG(rc);
                    goto RELEASE;
                }
            }
            /* notify the user, if requested */
            if (NULL != triplet->cbfunc) {
                ORCM_PNP_DECOMPOSE_STRING_ID(string_id, app, version, release);
                triplet->cbfunc(app, version, release, output);
                triplet->cbfunc = NULL;
                if (NULL != app) {
                    free(app);
                }
                if (NULL != version) {
                    free(version);
                }
                if (NULL != release) {
                    free(release);
                }
            }
            goto recvs;
        }
    }
    
    /* if we get here, then this is a new application
     * triplet - add it to our list
     */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received_announcement has new triplet",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    triplet = OBJ_NEW(orcm_pnp_triplet_t);
    triplet->string_id = strdup(string_id);
    triplet->channel = output;
    opal_pointer_array_add(&triplets, triplet);

recvs:
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement from source %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(&sender)));
    
    /* do we already know this source? */
    source = (orcm_pnp_source_t*)opal_pointer_array_get_item(&triplet->members, sender.vpid);
    if (NULL == source) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received adding source %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&sender)));
        
        source = OBJ_NEW(orcm_pnp_source_t);
        source->name.jobid = sender.jobid;
        source->name.vpid = sender.vpid;
        opal_pointer_array_set_item(&triplet->members, sender.vpid, source);
        known = false;
        if (ORTE_SUCCESS != (rc = orte_rml.set_contact_info(rml_uri))) {
            ORTE_ERROR_LOG(rc);
            goto RELEASE;
        }        
    }

    /* who are they responding to? */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &originator, &n, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        goto RELEASE;
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
        goto RELEASE;
    }
    
    /* if we get here, then this is an original announcement */
    if (ORCM_PROC_IS_APP) {
        chan = ORTE_RMCAST_APP_PUBLIC_CHANNEL;
    } else {
        chan = ORTE_RMCAST_SYS_CHANNEL;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement sending response",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* assemble the announcement response */
    OBJ_CONSTRUCT(&ann, opal_buffer_t);
    
    /* pack the common elements */
    if (ORCM_SUCCESS != (rc = pack_announcement(&ann, &sender))) {
        if (ORCM_ERR_NOT_AVAILABLE != rc) {
            /* not-avail => have not announced ourselves yet */
            ORTE_ERROR_LOG(rc);
        }
        OBJ_DESTRUCT(&ann);
        goto RELEASE;
    }
    
    /* send it - have to release the thread in case we receive something right away */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    if (ORCM_SUCCESS != (rc = orte_rmcast.send_buffer(chan,
                                                      ORTE_RMCAST_TAG_ANNOUNCE,
                                                      &ann))) {
        ORTE_ERROR_LOG(rc);
    }
    
    /* cleanup */
    OBJ_DESTRUCT(&ann);
    goto CALLBACK;
    
RELEASE:
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
CALLBACK:
    /* if this is a new source and they wanted a callback,
     * now is the time to do it */
    if (!known && NULL != my_announce_cbfunc) {
        /* break the stringid into its elements */
        ORCM_PNP_DECOMPOSE_STRING_ID(string_id, app, version, release);
        my_announce_cbfunc(app, version, release, &sender, nodename, uid);
        if (NULL != app) {
            free(app);
        }
        if (NULL != version) {
            free(version);
        }
        if (NULL != release) {
            free(release);
        }
    }
    if (NULL != string_id) {
        free(string_id);
    }
    if (NULL != rml_uri) {
        free(rml_uri);
    }
    return;
}

static void recv_input_buffers(int status,
                               orte_rmcast_channel_t channel,
                               orte_rmcast_tag_t tag,
                               orte_process_name_t *sender,
                               opal_buffer_t *buf, void *cbdata)
{
    orcm_pnp_triplet_t *trp;
    orcm_pnp_source_t *src;
    orcm_pnp_request_t *request;
    opal_list_item_t *item;
    int i, rc;
    char *string_id;
    int32_t n, num_iovecs, num_bytes;
    struct iovec *iovecs;
    int8_t flag;
    orcm_pnp_tag_t tg;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received input buffer on channel %d tag %d from sender %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), channel, tag,
                         ORTE_NAME_PRINT(sender)));
    
    /* protect against threading */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* extract the string id of the sender */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &string_id, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        goto DEPART;
    }    
    
    /* extract the pnp tag */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &tg, &n, ORCM_PNP_TAG_T))) {
        ORTE_ERROR_LOG(rc);
        goto DEPART;
    }    

    /* find this triplet */
    trp = NULL;
    for (i=0; i < triplets.size; i++) {
        if (NULL == (trp = (orcm_pnp_triplet_t*)opal_pointer_array_get_item(&triplets, i))) {
            continue;
        }
        if (triplet_cmp(string_id, trp->string_id)) {
            /* we have a match */
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s pnp:default:recv triplet %s found",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), string_id));
            break;
        }
    }
    if (NULL == trp) {
        /* we don't know this channel yet - just ignore it */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:recv triplet %s unknown",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), string_id));
        free(string_id);
        goto DEPART;
    }
    free(string_id);
    
    /* find the request object for this tag */
    if (NULL == (request = find_request(&trp->requests, tg))) {
        /* no matching requests */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:recv triplet %s has no requested recvs",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), trp->string_id));
        goto DEPART;
    }
    
    if (NULL == (src = (orcm_pnp_source_t*)opal_pointer_array_get_item(&trp->members, sender->vpid))) {
        /* don't know this sender - add it */
        src = OBJ_NEW(orcm_pnp_source_t);
        src->name.jobid = sender->jobid;
        src->name.vpid = sender->vpid;
        opal_pointer_array_set_item(&trp->members, src->name.vpid, src);
    }
    
    /* unpack the iovec vs buffer flag */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &flag, &n, OPAL_INT8))) {
        ORTE_ERROR_LOG(rc);
        goto DEPART;
    }    
    
    if (NULL != request->cbfunc && 0 == flag) {
        /* iovecs were requested and sent - get them */
        n=1;
        if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &num_iovecs, &n, OPAL_INT32))) {
            ORTE_ERROR_LOG(rc);
            goto DEPART;
        }
        if (0 < num_iovecs) {
            iovecs = (struct iovec *)malloc(num_iovecs * sizeof(struct iovec));
            for (i=0; i < num_iovecs; i++) {
                n=1;
                if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &num_bytes, &n, OPAL_INT32))) {
                    ORTE_ERROR_LOG(rc);
                    goto DEPART;
                }
                iovecs[i].iov_len = num_bytes;
                if (0 < num_bytes) {
                    iovecs[i].iov_base = (uint8_t*)malloc(num_bytes);
                    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, iovecs[i].iov_base, &num_bytes, OPAL_UINT8))) {
                        ORTE_ERROR_LOG(rc);
                        goto DEPART;
                    }
                } else {
                    iovecs[i].iov_base = NULL;
                }
            }
        }
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received input iovecs - delivering msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        /* release the thread prior to executing the callback
         * to avoid deadlock
         */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        request->cbfunc(ORCM_SUCCESS, sender, tag, iovecs, num_iovecs, NULL);
        return;
    }

    if (NULL != request->cbfunc_buf && 1 == flag) {
        /* buffer was requested and sent - just hand it over */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received input buffer - delivering msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        /* release the thread prior to executing the callback
         * to avoid deadlock
         */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        request->cbfunc_buf(ORCM_SUCCESS, sender, tag, buf, NULL);
        return;
    }
    /* if no callback provided, then just ignore the message */
    
DEPART:
    /* release the thread */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
}

/* pack the common elements of an announcement message */
static int pack_announcement(opal_buffer_t *buf, orte_process_name_t *sender)
{
    int ret;
    char *rml_uri;
    
    /* if we haven't registered an app-triplet yet, then we can't announce */
    if (NULL == my_string_id) {
        return ORCM_ERR_NOT_AVAILABLE;
    }
    
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, ORTE_PROC_MY_NAME, 1, ORTE_NAME))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_string_id, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    /* pack my output channel */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_channel, 1, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    /* pack my node */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &orte_process_info.nodename, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    /* pack my userid */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_uid, 1, OPAL_UINT32))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    /* pack my uri */
    rml_uri = orte_rml.get_contact_info();
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &rml_uri, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    free(rml_uri);
    
    /* tell everyone we are responding to an announcement
     * so they don't respond back
     */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, sender, 1, ORTE_NAME))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }        
    
    if (ORCM_PROC_IS_DAEMON || ORCM_PROC_IS_MASTER) {
        /* if we are a daemon or the master, include our system info */
        /* get our local resources */
        char *keys[] = {
            OPAL_SYSINFO_CPU_TYPE,
            OPAL_SYSINFO_CPU_MODEL,
            OPAL_SYSINFO_NUM_CPUS,
            OPAL_SYSINFO_MEM_SIZE,
            NULL
        };
        opal_list_t resources;
        opal_list_item_t *item;
        opal_sysinfo_value_t *info;
        int32_t num_values;
        
        /* include our node name */
        opal_dss.pack(buf, &orte_process_info.nodename, 1, OPAL_STRING);
        
        OBJ_CONSTRUCT(&resources, opal_list_t);
        opal_sysinfo.query(keys, &resources);
        /* add number of values to the buffer */
        num_values = opal_list_get_size(&resources);
        opal_dss.pack(buf, &num_values, 1, OPAL_INT32);
        /* add them to the buffer */
        while (NULL != (item = opal_list_remove_first(&resources))) {
            info = (opal_sysinfo_value_t*)item;
            opal_dss.pack(buf, &info->key, 1, OPAL_STRING);
            opal_dss.pack(buf, &info->type, 1, OPAL_DATA_TYPE_T);
            if (OPAL_INT64 == info->type) {
                opal_dss.pack(buf, &(info->data.i64), 1, OPAL_INT64);
            } else if (OPAL_STRING == info->type) {
                opal_dss.pack(buf, &(info->data.str), 1, OPAL_STRING);
            }
            OBJ_RELEASE(info);
        }
        OBJ_DESTRUCT(&resources);
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

static void rml_callback_buffer(int status,
                                orte_process_name_t* sender,
                                opal_buffer_t* buffer,
                                orte_rml_tag_t tag,
                                void* cbdata)
{
    orcm_pnp_send_t *send = (orcm_pnp_send_t*)cbdata;
    
    /* do any required callbacks */
    if (NULL != send->cbfunc_buf) {
        send->cbfunc_buf(status, ORTE_PROC_MY_NAME, send->tag, send->buffer, send->cbdata);
    }
    OBJ_RELEASE(send);
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
    orcm_pnp_request_t *request;
    char *string_id;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default recvd direct msg from %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));
    
    /* protect against threading */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    if (NULL == my_string_id) {
        /* we haven't announced ourselves yet, so this should never happen */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return;
    }
    
    /* unpack the sender's string id */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buffer, &string_id, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    
    /* unpack the intended tag for this message */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buffer, &tag, &n, ORCM_PNP_TAG_T))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    
    /* unpack the flag */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buffer, &flag, &n, OPAL_INT8))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    
    /* find the request object for this tag */
    if (NULL == (request = find_request(&my_triplet->requests, tag))) {
        /* no matching requests */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received no matching request for tag %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), tag));
        goto CLEANUP;
    }
    
    if (1 == flag && NULL != request->cbfunc_buf) {
        /* release the thread prior to executing the callback
         * to avoid deadlock
         */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        /* deliver the message */
        request->cbfunc_buf(ORCM_SUCCESS, sender, tag, buffer, NULL);
        goto DEPART;
    } else if (0 == flag && NULL != request->cbfunc) {
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
            if (0 < sz) {
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
        }
        /* release the thread prior to executing the callback
         * to avoid deadlock
         */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        /* deliver the message */
        request->cbfunc(ORCM_SUCCESS, sender, tag, iovec_array, iovec_count, NULL);
        goto DEPART;
    } else {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received no matching cbfunc for specified data type %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (1 == flag) ? "buffer" : "iovec"));
    }
    
    
CLEANUP:
    /* release the thread */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
DEPART:
    /* reissue the recv */
    if (ORTE_SUCCESS != (rc = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD,
                                                      ORTE_RML_TAG_MULTICAST_DIRECT,
                                                      ORTE_RML_NON_PERSISTENT,
                                                      recv_direct_msgs,
                                                      NULL))) {
        ORTE_ERROR_LOG(rc);
    }
    
}

static orcm_pnp_triplet_t* get_triplet(char *app,
                                       char *version,
                                       char *release)
{
    int i;
    orcm_pnp_triplet_t *triplet;
    char *string_id;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:get_triplet app %s version %s release %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release));
    
    ORCM_PNP_CREATE_STRING_ID(&string_id, app, version, release);

    for (i=0; i < triplets.size; i++) {
        if (NULL == (triplet = (orcm_pnp_triplet_t*)opal_pointer_array_get_item(&triplets, i))) {
            continue;
        }
        if (0 == strcasecmp(string_id, triplet->string_id)) {
            /* we have a match */
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s pnp:default:get_triplet match found",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
            free(string_id);
            return triplet;
        }
    }
    
    /* if we get here, then this triplet doesn't exist - create it */
    triplet = OBJ_NEW(orcm_pnp_triplet_t);
    triplet->string_id = strdup(string_id);
    free(string_id);
    
    opal_pointer_array_add(&triplets, triplet);
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:get_triplet created triplet %s channel %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         triplet->string_id, triplet->channel));
    
    return triplet;
}

static void setup_recv_request(orcm_pnp_triplet_t *triplet,
                               orcm_pnp_tag_t tag,
                               orcm_pnp_callback_fn_t cbfunc,
                               orcm_pnp_callback_buffer_fn_t cbfunc_buf)
{
    orcm_pnp_request_t *request;
    
    if (NULL == (request = find_request(&triplet->requests, tag))) {
        request = OBJ_NEW(orcm_pnp_request_t);
        request->tag = tag;
        /* ensure that any wildcard tag is at the end of the list */
        if (ORCM_PNP_TAG_WILDCARD == tag) {
            opal_list_append(&triplet->requests, &request->super);
        } else {
            opal_list_prepend(&triplet->requests, &request->super);
        }
    }
    
    /* setup the callback functions - since the request may
     * be pre-existing, we need to be careful that we only
     * update the functions that were provided
     */
    if (NULL != cbfunc) {
        request->cbfunc = cbfunc;
    }
    if (NULL != cbfunc_buf) {
        request->cbfunc_buf = cbfunc_buf;
    }    
}

static orcm_pnp_request_t* find_request(opal_list_t *list,
                                        orcm_pnp_tag_t tag)
{
    orcm_pnp_request_t *req=NULL;
    opal_list_item_t *item;
    
    for (item = opal_list_get_first(list);
         item != opal_list_get_end(list);
         item = opal_list_get_next(item)) {
        req = (orcm_pnp_request_t*)item;
        
        if (tag == req->tag) {
            return req;
        }
    }
    
    /* see if the last entry is the wildcard tag */
    if (NULL != req && ORCM_PNP_TAG_WILDCARD == req->tag) {
        return req;
    }
    
    return NULL;
}

static int construct_msg(opal_buffer_t **buf, opal_buffer_t *buffer,
                         orcm_pnp_tag_t tag, struct iovec *msg, int count)
{
    int ret;
    int8_t flag;
    int sz;
    int32_t cnt;

    *buf = OBJ_NEW(opal_buffer_t);
    
    /* insert our string_id */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(*buf, &my_string_id, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
        OBJ_RELEASE(*buf);
        return ret;
    }

    /* pack the tag - we don't actually need the tag for messages sent
     * via multicast as we get it directly passed to the callback function.
     * However, we don't get the right tag passed to us for direct
     * messages, so we need it there. Since it costs next to nothing to
     * include it, we do so for all cases
     */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(*buf, &tag, 1, ORCM_PNP_TAG_T))) {
        ORTE_ERROR_LOG(ret);
        OBJ_RELEASE(*buf);
        return ret;
    }
    
    if (NULL != msg) {
        /* flag the buffer as containing iovecs */
        flag = 0;
        if (ORCM_SUCCESS != (ret = opal_dss.pack(*buf, &flag, 1, OPAL_INT8))) {
            ORTE_ERROR_LOG(ret);
            OBJ_RELEASE(*buf);
            return ret;
        }    
        /* pack the number of iovecs */
        cnt = count;
        if (ORCM_SUCCESS != (ret = opal_dss.pack(*buf, &cnt, 1, OPAL_INT32))) {
            ORTE_ERROR_LOG(ret);
            OBJ_RELEASE(*buf);
            return ret;
        }
        
        /* pack each iovec into a buffer in prep for sending
         * so we can recreate the array at the other end
         */
        for (sz=0; sz < count; sz++) {
            /* pack the size */
            cnt = msg[sz].iov_len;
            if (ORCM_SUCCESS != (ret = opal_dss.pack(*buf, &cnt, 1, OPAL_INT32))) {
                ORTE_ERROR_LOG(ret);
                OBJ_RELEASE(*buf);
                return ret;
            }        
            if (0 < cnt) {
                /* pack the bytes */
                if (ORCM_SUCCESS != (ret = opal_dss.pack(*buf, msg[sz].iov_base, cnt, OPAL_UINT8))) {
                    ORTE_ERROR_LOG(ret);
                    OBJ_RELEASE(*buf);
                    return ret;
                }            
            }
        }
        return ORCM_SUCCESS;
    }
    
    /* flag that we sent a buffer */
    flag = 1;
    if (ORCM_SUCCESS != (ret = opal_dss.pack(*buf, &flag, 1, OPAL_INT8))) {
        ORTE_ERROR_LOG(ret);
        OBJ_RELEASE(*buf);
        return ret;
    }    
    /* copy the payload */
    if (ORTE_SUCCESS != (ret = opal_dss.copy_payload(*buf, buffer))) {
        ORTE_ERROR_LOG(ret);
        OBJ_RELEASE(*buf);
    }
    return ret;
}

static bool triplet_cmp(char *str1, char *str2)
{
    char *a1, *v1, *r1;
    char *a2, *v2, *r2;
    
    ORCM_PNP_DECOMPOSE_STRING_ID(str1, a1, v1, r1);
    ORCM_PNP_DECOMPOSE_STRING_ID(str2, a2, v2, r2);

    if (NULL == a1 || NULL == a2) {
        /* we automatically match on this field */
        goto check_version;
    }
    if (0 != strcasecmp(a1, a2)) {
        return false;
    }
    
check_version:
    if (NULL == v1 || NULL == v2) {
        /* we automatically match on this field */
        goto check_release;
    }
    if (0 != strcasecmp(v1, v2)) {
        return false;
    }
    
check_release:
    if (NULL == v1 || NULL == v2) {
        /* we automatically match on this field */
        return true;
    }
    if (0 == strcasecmp(r1, r2)) {
        return true;
    }
    return false;
}
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
#include "opal/util/argv.h"
#include "opal/util/output.h"
#include "opal/mca/sysinfo/sysinfo.h"

#include "orte/mca/rmcast/rmcast.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/routed/routed.h"
#include "orte/runtime/orte_globals.h"
#include "orte/threads/threads.h"

#include "mca/leader/leader.h"
#include "runtime/runtime.h"
#include "util/triplets.h"

#include "mca/pnp/pnp.h"
#include "mca/pnp/base/private.h"
#include "mca/pnp/base/public.h"
#include "mca/pnp/default/pnp_default.h"

/* API functions */

static int default_init(void);
static int announce(const char *app,
                    const char *version,
                    const char *release,
                    orcm_pnp_announce_fn_t cbfunc);
static int open_channel(const char *app,
                        const char *version,
                        const char *release,
                        orte_jobid_t jobid,
                        orcm_pnp_open_channel_cbfunc_t cbfunc);
static int register_receive(const char *app,
                            const char *version,
                            const char *release,
                            orcm_pnp_channel_t channel,
                            orcm_pnp_tag_t tag,
                            orcm_pnp_callback_fn_t cbfunc,
                            void *cbdata);
static int cancel_receive(const char *app,
                          const char *version,
                          const char *release,
                          orcm_pnp_channel_t channel,
                          orcm_pnp_tag_t tag);
static int default_output(orcm_pnp_channel_t channel,
                          orte_process_name_t *recipient,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg, int count,
                          opal_buffer_t *buffer);
static int default_output_nb(orcm_pnp_channel_t channel,
                             orte_process_name_t *recipient,
                             orcm_pnp_tag_t tag,
                             struct iovec *msg, int count,
                             opal_buffer_t *buffer,
                             orcm_pnp_callback_fn_t cbfunc,
                             void *cbdata);
static orcm_pnp_tag_t define_new_tag(void);
static char* get_string_id(void);
static int disable_comm(void);
static int default_finalize(void);

/* The module struct */

orcm_pnp_base_module_t orcm_pnp_default_module = {
    default_init,
    announce,
    open_channel,
    register_receive,
    cancel_receive,
    default_output,
    default_output_nb,
    define_new_tag,
    get_string_id,
    disable_comm,
    default_finalize
};

/* Local functions */
static void recv_announcements(orte_process_name_t *sender,
                               opal_buffer_t *buf);
static void recv_input_buffers(int status,
                               orte_rmcast_channel_t channel,
                               orte_rmcast_seq_t seq_num,
                               orte_rmcast_tag_t tag,
                               orte_process_name_t *sender,
                               opal_buffer_t *buf, void *cbdata);
static int pack_announcement(opal_buffer_t *buf, orte_process_name_t *name);

static void rmcast_callback(int status,
                            orte_rmcast_channel_t channel,
                            orte_rmcast_seq_t seq_num,
                            orte_rmcast_tag_t tag,
                            orte_process_name_t *sender,
                            opal_buffer_t *buf, void* cbdata);

static void rml_callback(int status,
                         struct orte_process_name_t* peer,
                         struct opal_buffer_t* buffer,
                         orte_rml_tag_t tag,
                         void* cbdata);

static void* recv_messages(opal_object_t *obj);

static void recv_direct_msgs(int status, orte_process_name_t* sender,
                             opal_buffer_t* buffer, orte_rml_tag_t tag,
                             void* cbdata);

static void check_pending_recvs(orcm_triplet_t *trp, orcm_triplet_group_t *grp);
static void update_pending_recvs(orcm_triplet_t *trp);

static int record_recv(orcm_triplet_t *triplet,
                       orcm_pnp_channel_t channel,
                       orcm_pnp_tag_t tag,
                       orcm_pnp_callback_fn_t cbfunc,
                       void *cbdata);

static orcm_pnp_request_t* find_request(opal_list_t *list,
                                        char *string_id,
                                        orcm_pnp_tag_t tag);

static int construct_msg(opal_buffer_t **buf, opal_buffer_t *buffer,
                         orcm_pnp_tag_t tag, struct iovec *msg, int count);

static void process_msg(orcm_pnp_msg_t *msg);
static void* deliver_msg(opal_object_t *obj);

/* Local variables */
static opal_pointer_array_t channels;
static int32_t packet_number = 0;
static bool recv_on = false;
static char *my_string_id = NULL;
static uint32_t my_uid;
static orcm_pnp_announce_fn_t my_announce_cbfunc = NULL;
static orcm_pnp_channel_obj_t *my_output_channel=NULL, *my_input_channel=NULL;
static orcm_triplet_t *my_triplet=NULL;
static orcm_triplet_group_t *my_group=NULL;

/* local thread support */
static orte_thread_ctl_t local_thread;
static orte_thread_ctl_t msg_ctl;
static opal_thread_t msg_thread;
static opal_list_t msg_delivery;
static volatile bool msg_thread_end=false;

static int default_init(void)
{
    int ret;
    orcm_pnp_channel_obj_t *chan;
    orte_rmcast_channel_t channel;
    orte_rmcast_channel_t input, output;
    char *stringid;
    orcm_pnp_request_t *req;

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "pnp default init"));
    
    /* sanity check in case ORTE was built without --enable-multicast */
    if (NULL == orte_rmcast.open_channel) {
        orte_show_help("help-pnp-default.txt", "multicast-disabled", true);
        return ORCM_ERR_NOT_SUPPORTED;
    }
    
    /* setup the threading support */
    OBJ_CONSTRUCT(&local_thread, orte_thread_ctl_t);
    /* setup the msg threading support */
    OBJ_CONSTRUCT(&msg_thread, opal_thread_t);
    msg_thread.t_run = deliver_msg;
    OBJ_CONSTRUCT(&msg_ctl, orte_thread_ctl_t);
    OBJ_CONSTRUCT(&msg_delivery, opal_list_t);

    /* init the array of channels */
    OBJ_CONSTRUCT(&channels, opal_pointer_array_t);
    opal_pointer_array_init(&channels, 8, INT_MAX, 8);

    /* record my channels */
    if (ORTE_SUCCESS != (ret = orte_rmcast.query_channel(&output, &input))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    
    if (ORCM_PNP_INVALID_CHANNEL != output) {
        my_output_channel = OBJ_NEW(orcm_pnp_channel_obj_t);
        my_output_channel->channel = output;
        opal_pointer_array_set_item(&channels, output, my_output_channel);
    }

    if (ORCM_PNP_INVALID_CHANNEL != input && output != input) {
        my_input_channel = OBJ_NEW(orcm_pnp_channel_obj_t);
        my_input_channel->channel = input;
        opal_pointer_array_set_item(&channels, input, my_input_channel);
    }

    /* record my uid */
    my_uid = (uint32_t)getuid();

    /* open the default channel */
    if (ORCM_PROC_IS_MASTER || ORCM_PROC_IS_DAEMON || ORCM_PROC_IS_TOOL) {
        channel = ORTE_RMCAST_SYS_CHANNEL;
        stringid = "ORCM:DVM:SYSTEM";
    } else {
        channel = ORTE_RMCAST_APP_PUBLIC_CHANNEL;
        stringid = "ORCM:APP:PUBLIC";
    }

    if (NULL != my_input_channel && my_input_channel->channel == channel) {
        chan = my_input_channel;
    } else if (NULL != my_output_channel && my_output_channel->channel == channel) {
        chan = my_output_channel;
        /* for tools and system utilities, no input channel is assigned,
         * so set their input to be their output so direct messages sent
         * to them have a place to go
         */
        if (NULL == my_input_channel) {
            my_input_channel = my_output_channel;
        }
    } else {
        chan = OBJ_NEW(orcm_pnp_channel_obj_t);
        chan->channel = channel;
        opal_pointer_array_set_item(&channels, channel, chan);
    }
    
    /* startup the msg thread */
    if (orcm_pnp_base.use_threads) {
        if (OPAL_SUCCESS != (ret = opal_thread_start(&msg_thread))) {
            opal_output(0, "%s Unable to start message delivery thread",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
            return ret;
        }
    }

    /* open the default "system" channel */
    if (ORCM_SUCCESS != (ret = orte_rmcast.open_channel(chan->channel,
                                                        stringid,
                                                        NULL, -1, NULL,
                                                        ORTE_RMCAST_BIDIR))) {
        if (ORTE_EXISTS != ret) {
            ORTE_ERROR_LOG(ret);
            return ret;
        }
    }
    /* setup to listen to it - will just return if we already are */
    if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(chan->channel,
                                                          ORTE_RMCAST_TAG_WILDCARD,
                                                          ORTE_RMCAST_PERSISTENT,
                                                          recv_input_buffers, NULL))) {
        if (ORTE_EXISTS != ret) {
            ORTE_ERROR_LOG(ret);
            return ret;
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

    return ORCM_SUCCESS;
}

static int announce(const char *app,
                    const char *version,
                    const char *release,
                    orcm_pnp_announce_fn_t cbfunc)
{
    int ret;
    opal_buffer_t buf;
    orcm_pnp_channel_t chan;

    /* bozo check */
    if (NULL == app || NULL == version || NULL == release) {
        ORTE_ERROR_LOG(ORCM_ERR_BAD_PARAM);
        return ORCM_ERR_BAD_PARAM;
    }
    
    /* protect against threading */
    ORTE_ACQUIRE_THREAD(&local_thread);
    
    if (NULL != my_string_id) {
        /* must have been called before */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:announce called before",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        ORTE_RELEASE_THREAD(&local_thread);
        return ORCM_SUCCESS;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:announce app %s version %s release %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         app, version, release));
    
    /* retain a local record of my info - this enables communication
     * by setting my_string_id != NULL
     */
    ORCM_CREATE_STRING_ID(&my_string_id, app, version, release);
    
    /* retain the callback function */
    my_announce_cbfunc = cbfunc;
    
    /* get a triplet object for myself - creates
     * it if one doesn't already exist
     */
    my_triplet = orcm_get_triplet(app, version, release, true);
    /* get my group object */
    my_group = orcm_get_triplet_group(my_triplet, ORTE_PROC_MY_NAME->jobid, true);
    my_group->uid = my_uid;
    my_group->input = my_input_channel->channel;
    my_group->output = my_output_channel->channel;

    /* check for pending recvs for these channels - this will copy
     * recvs that were pre-posted on the triplet to the channel
     * array
     */
    check_pending_recvs(my_triplet, my_group);

    /* release the triplet as we no longer require it */
    ORTE_RELEASE_THREAD(&my_triplet->ctl);

    /* no need to hold the lock any further */
    ORTE_RELEASE_THREAD(&local_thread);
    
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
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:announce sending",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    if (ORCM_SUCCESS != (ret = default_output(chan, NULL,
                                              ORCM_PNP_TAG_ANNOUNCE,
                                              NULL, 0, &buf))) {
        ORTE_ERROR_LOG(ret);
    }
    
    /* cleanup */
    OBJ_DESTRUCT(&buf);
    
    return ret;
}

static int open_channel(const char *app,
                        const char *version,
                        const char *release,
                        orte_jobid_t jobid,
                        orcm_pnp_open_channel_cbfunc_t cbfunc)
{
    orcm_triplet_t *triplet;
    orcm_triplet_group_t *grp;
    orcm_pnp_request_t *request;
    opal_list_item_t *item;
    int i, rc;
    bool done;
    
    if (NULL == cbfunc) {
        /* makes no sense */
        ORTE_ERROR_LOG(ORCM_ERR_BAD_PARAM);
        return ORCM_ERR_BAD_PARAM;
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:open channel for app %s version %s release %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         app, version, release));
    
    /* protect the global arrays */
    ORTE_ACQUIRE_THREAD(&local_thread);
    
    /* see if we already know this triplet - automatically
     * creates it if not
     */
    triplet = orcm_get_triplet(app, version, release, true);
    /* record the policy */
    triplet->pnp_cb_policy = jobid;
    triplet->pnp_cbfunc = cbfunc;

    /* if the jobid is wildcard, we execute the callback for every group */
    if (ORTE_JOBID_WILDCARD == jobid) {
        /* cycle thru this triplet's known groups */
        for (i=0; i < triplet->groups.size; i++) {
            if (NULL == (grp = (orcm_triplet_group_t*)opal_pointer_array_get_item(&triplet->groups, i))) {
                continue;
            }
            grp->pnp_cbfunc = cbfunc;
            if (ORCM_PNP_INVALID_CHANNEL != grp->input) {
                /* release the threads before doing the callback in
                 * case the caller sends messages
                 */
                OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                     "%s pnp:default:open channel executing callback for jobid %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_JOBID_PRINT(grp->jobid)));
                ORTE_RELEASE_THREAD(&triplet->ctl);
                ORTE_RELEASE_THREAD(&local_thread);
                cbfunc(app, version, release, grp->input);
                /* reacquire the threads */
                ORTE_ACQUIRE_THREAD(&local_thread);
                ORTE_ACQUIRE_THREAD(&triplet->ctl);
                /* flag that this group has executed its callback */
                grp->pnp_cbfunc = NULL;
            }
        }
        /* release the threads */
        ORTE_RELEASE_THREAD(&triplet->ctl);
        ORTE_RELEASE_THREAD(&local_thread);
        return ORCM_SUCCESS;
    }

    if (ORTE_JOBID_INVALID == jobid) {
        /* see if we know about any group with this triplet */
        done = false;
        for (i=0; i < triplet->groups.size; i++) {
            if (NULL == (grp = (orcm_triplet_group_t*)opal_pointer_array_get_item(&triplet->groups, i))) {
                continue;
            }
            grp->pnp_cbfunc = cbfunc;
            if (ORCM_PNP_INVALID_CHANNEL != grp->input) {
                /* flag that we already did the callback so we don't do it again */
                OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                     "%s pnp:default:open channel executing callback for jobid %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_JOBID_PRINT(grp->jobid)));
                done = true;
                ORTE_RELEASE_THREAD(&triplet->ctl);
                ORTE_RELEASE_THREAD(&local_thread);
                cbfunc(app, version, release, grp->input);
                /* reacquire the threads */
                ORTE_ACQUIRE_THREAD(&local_thread);
                ORTE_ACQUIRE_THREAD(&triplet->ctl);
                break;
            }
        }
        /* if we did the callback, remove any remaining cbfunc entries to ensure
         * that we only do this once for the triplet
         */
        if (done) {
            for (i=0; i < triplet->groups.size; i++) {
                if (NULL == (grp = (orcm_triplet_group_t*)opal_pointer_array_get_item(&triplet->groups, i))) {
                    continue;
                }
                grp->pnp_cbfunc = NULL;
            }
        }
        /* release the threads */
        ORTE_RELEASE_THREAD(&triplet->ctl);
        ORTE_RELEASE_THREAD(&local_thread);
        return ORCM_SUCCESS;
    }

    /* left with the case of a specific jobid - record the policy */
    done = false;
    for (i=0; i < triplet->groups.size; i++) {
        if (NULL == (grp = (orcm_triplet_group_t*)opal_pointer_array_get_item(&triplet->groups, i))) {
            continue;
        }
        if (grp->jobid == jobid) {
            /* found the group */
            grp->pnp_cbfunc = cbfunc;
            done = true;  /* flag that we found the group */
            if (ORCM_PNP_INVALID_CHANNEL != grp->input) {
                /* release the threads before doing the callback in
                 * case the caller sends messages
                 */
                OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                     "%s pnp:default:open channel executing callback for jobid %s",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     ORTE_JOBID_PRINT(grp->jobid)));
                ORTE_RELEASE_THREAD(&triplet->ctl);
                ORTE_RELEASE_THREAD(&local_thread);
                cbfunc(app, version, release, grp->input);
                /* reacquire the threads */
                ORTE_ACQUIRE_THREAD(&local_thread);
                ORTE_ACQUIRE_THREAD(&triplet->ctl);
                /* flag that this group has executed its callback */
                grp->pnp_cbfunc = NULL;
                break;
            }
        }
    }
    /* if we didn't find the group, then we have to add it */
    if (!done) {
        grp = OBJ_NEW(orcm_triplet_group_t);
        grp->jobid = jobid;
        grp->pnp_cbfunc = cbfunc;
    }

    ORTE_RELEASE_THREAD(&triplet->ctl);
    ORTE_RELEASE_THREAD(&local_thread);
    return ORCM_SUCCESS;
}

static int register_receive(const char *app,
                            const char *version,
                            const char *release,
                            orcm_pnp_channel_t channel,
                            orcm_pnp_tag_t tag,
                            orcm_pnp_callback_fn_t cbfunc,
                            void *cbdata)
{
    orcm_triplet_t *triplet, *trp;
    int i;
    int ret=ORCM_SUCCESS;
    orcm_pnp_channel_obj_t *recvr;
    orcm_pnp_request_t *req;

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:register_recv app %s version %s release %s channel %s tag %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release,
                         orcm_pnp_print_channel(channel),
                         orcm_pnp_print_tag(tag)));
    
    /* bozo check - can't receive on an invalid channel */
    if (ORCM_PNP_INVALID_CHANNEL == channel) {
        ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
        return ORTE_ERR_BAD_PARAM;
    }

    /* since we are modifying global lists, lock
     * the thread
     */
    ORTE_ACQUIRE_THREAD(&local_thread);
    
    /* get a triplet object for this triplet - creates
     * it if one doesn't already exist
     */
    triplet = orcm_get_triplet(app, version, release, true);        
    
    /* if the triplet involves wildcards, we treat it separately. Such
     * recvs are maintained on a separate list so they can be properly
     * applied to any subsequent triplets covered by the wildcard
     */
    if (NULL != strchr(triplet->string_id, '@')) {
        /* if we were given an INPUT or OUTPUT channel, then we
         * have to record the recv so we can apply it to triplets
         * as they become known since we don't know the channel
         */
        if (ORCM_PNP_GROUP_INPUT_CHANNEL == channel ||
            ORCM_PNP_GROUP_OUTPUT_CHANNEL == channel) {
            /* store this recv on this wildcard triplet so we retain a record of it,
             * ensuring no duplicates
             */
            if (ORCM_PNP_GROUP_INPUT_CHANNEL == channel) {
                if (NULL == find_request(&triplet->input_recvs, triplet->string_id, tag)) {
                    /* create it */
                    req = OBJ_NEW(orcm_pnp_request_t);
                    req->string_id = strdup(triplet->string_id);
                    req->tag = tag;
                    req->cbfunc = cbfunc;
                    req->cbdata = cbdata;
                    opal_list_append(&triplet->input_recvs, &req->super);
                }
            } else {
                if (NULL == find_request(&triplet->output_recvs, triplet->string_id, tag)) {
                    /* create it */
                    req = OBJ_NEW(orcm_pnp_request_t);
                    req->string_id = strdup(triplet->string_id);
                    req->tag = tag;
                    req->cbfunc = cbfunc;
                    req->cbdata = cbdata;
                    opal_list_append(&triplet->output_recvs, &req->super);
                }
            }

            /* lock the global triplet arrays for our use */
            ORTE_ACQUIRE_THREAD(&orcm_triplets->ctl);

            /* check all known triplets to find those that match */
            for (i=0; i < orcm_triplets->array.size; i++) {
                if (NULL == (trp = (orcm_triplet_t*)opal_pointer_array_get_item(&orcm_triplets->array, i))) {
                    continue;
                }
                /* lock the triplet thread */
                ORTE_ACQUIRE_THREAD(&trp->ctl);
                if (orcm_triplet_cmp(trp->string_id, triplet->string_id)) {
                    /* triplet matches - transfer the recv */
                    if (ORCM_SUCCESS != (ret = record_recv(trp, channel, tag, cbfunc, cbdata))) {
                        ORTE_ERROR_LOG(ret);
                    }
                }
                /* release this triplet */
                ORTE_RELEASE_THREAD(&trp->ctl);
            }

            /* release the global arrays */
            ORTE_RELEASE_THREAD(&orcm_triplets->ctl);
        } else {
            /* if we were given a specific channel, then we can add this
             * recv to it
             */
            if (NULL == (recvr = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&channels, channel))) {
                recvr = OBJ_NEW(orcm_pnp_channel_obj_t);
                recvr->channel = channel;
                opal_pointer_array_set_item(&channels, recvr->channel, recvr);
            }
            if (NULL == (req = find_request(&recvr->recvs, triplet->string_id, tag))) {
                /* not already present - create it */
                req = OBJ_NEW(orcm_pnp_request_t);
                req->string_id = strdup(req->string_id);
                req->tag = req->tag;
                req->cbfunc = cbfunc;
                req->cbdata = cbdata;
                opal_list_append(&recvr->recvs, &req->super);
            }
            if (channel < ORCM_PNP_SYS_CHANNEL) {
                /* can't register rmcast recvs on group_input, group_output, and wildcard channels */
                goto cleanup;
            }
            /* open this channel - will just return if already open */
            if (ORCM_SUCCESS != (ret = orte_rmcast.open_channel(channel, triplet->string_id,
                                                                NULL, -1, NULL,
                                                                ORTE_RMCAST_RECV))) {
                if (ORTE_EXISTS != ret) {
                    ORTE_ERROR_LOG(ret);
                    goto cleanup;
                }
            }
            /* setup to listen to it - will just return if we already are */
            if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(channel, ORTE_RMCAST_TAG_WILDCARD,
                                                                  ORTE_RMCAST_PERSISTENT,
                                                                  recv_input_buffers, NULL))) {
                if (ORTE_EXISTS == ret) {
                    ret = ORTE_SUCCESS;
                    goto cleanup;
                }
                ORTE_ERROR_LOG(ret);
            }
        }

    } else {
        /* we are dealing with a non-wildcard triplet - record the request */
        if (ORCM_SUCCESS != (ret = record_recv(triplet, channel, tag, cbfunc, cbdata))) {
            ORTE_ERROR_LOG(ret);
        }
    }

 cleanup:
    /* clear the threads */
    ORTE_RELEASE_THREAD(&triplet->ctl);
    ORTE_RELEASE_THREAD(&local_thread);
    
    return ret;
}

static int cancel_receive(const char *app,
                          const char *version,
                          const char *release,
                          orcm_pnp_channel_t channel,
                          orcm_pnp_tag_t tag)
{
    orcm_pnp_channel_obj_t *chan;
    orcm_pnp_request_t *req;
    orcm_triplet_t *triplet;
    orcm_triplet_group_t *grp;
    opal_list_item_t *item, *next;
    char *string_id;
    int ret=ORCM_SUCCESS;
    int i;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:cancel_recv app %s version %s release %s channel %s tag %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release,
                         orcm_pnp_print_channel(channel),
                         orcm_pnp_print_tag(tag)));
    
    /* since we are modifying global lists, lock
     * the thread
     */
    ORTE_ACQUIRE_THREAD(&local_thread);
    
    /* if this is the wildcard channel, loop across all channels */
    if (ORCM_PNP_WILDCARD_CHANNEL == channel) {
        /* get the string id for this triplet */
        ORCM_CREATE_STRING_ID(&string_id, app, version, release);
        for (i=0; i < channels.size; i++) {
            if (NULL == (chan = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&channels, i))) {
                continue;
            }
            item = opal_list_get_first(&chan->recvs);
            while (item != opal_list_get_end(&chan->recvs)) {
                next = opal_list_get_next(item);
                req = (orcm_pnp_request_t*)item;
                if (0 == strcasecmp(string_id, ORCM_WILDCARD_STRING_ID) ||
                    0 == strcasecmp(string_id, req->string_id)) {
                    if (ORCM_PNP_TAG_WILDCARD == tag || tag == req->tag) {
                        opal_list_remove_item(&chan->recvs, item);
                        OBJ_RELEASE(item);
                    }
                }
                item = next;
            }
        }
        goto cleanup;
    }

    /* are we looking at the group input channel? */
    if (ORCM_PNP_GROUP_INPUT_CHANNEL == channel) {
        triplet = orcm_get_triplet(app, version, release, false);
        if (NULL != triplet) {
            /* remove the triplet-stored recvs */
            item = opal_list_get_first(&triplet->input_recvs);
            while (item != opal_list_get_end(&triplet->input_recvs)) {
                next = opal_list_get_next(item);
                req = (orcm_pnp_request_t*)item;
                if (ORCM_PNP_TAG_WILDCARD == tag || tag == req->tag) {
                    opal_list_remove_item(&triplet->input_recvs, item);
                    OBJ_RELEASE(item);
                }
                item = next;
            }
            for (i=0; i < triplet->groups.size; i++) {
                if (NULL == (grp = (orcm_triplet_group_t*)opal_pointer_array_get_item(&triplet->groups, i))) {
                    continue;
                }
                if (ORCM_PNP_INVALID_CHANNEL != grp->input) {
                    /* just look thru the default group input channel */
                    chan = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&channels, grp->input);
                    if (NULL == chan) {
                        /* nothing to do */
                        ORTE_RELEASE_THREAD(&triplet->ctl);
                        goto cleanup;
                    }
                    item = opal_list_get_first(&chan->recvs);
                    while (item != opal_list_get_end(&chan->recvs)) {
                        next = opal_list_get_next(item);
                        req = (orcm_pnp_request_t*)item;
                        if (ORCM_PNP_TAG_WILDCARD == tag || tag == req->tag) {
                            opal_list_remove_item(&chan->recvs, item);
                            OBJ_RELEASE(item);
                        }
                        item = next;
                    }
                }
            }
            /* release the triplet */
            ORTE_RELEASE_THREAD(&triplet->ctl);
        }
        goto cleanup;
    }

    /* are we looking at the group output channel? */
    if (ORCM_PNP_GROUP_OUTPUT_CHANNEL == channel) {
        triplet = orcm_get_triplet(app, version, release, false);
        if (NULL != triplet) {
            /* remove the triplet-stored recvs */
            item = opal_list_get_first(&triplet->output_recvs);
            while (item != opal_list_get_end(&triplet->output_recvs)) {
                next = opal_list_get_next(item);
                req = (orcm_pnp_request_t*)item;
                if (ORCM_PNP_TAG_WILDCARD == tag || tag == req->tag) {
                    opal_list_remove_item(&triplet->output_recvs, item);
                    OBJ_RELEASE(item);
                }
                item = next;
            }
            for (i=0; i < triplet->groups.size; i++) {
                if (NULL == (grp = (orcm_triplet_group_t*)opal_pointer_array_get_item(&triplet->groups, i))) {
                    continue;
                }
                if (ORCM_PNP_INVALID_CHANNEL != grp->output) {
                    /* just look thru the default group output channel */
                    chan = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&channels, grp->output);
                    if (NULL == chan) {
                        /* nothing to do */
                        ORTE_RELEASE_THREAD(&triplet->ctl);
                        goto cleanup;
                    }
                    item = opal_list_get_first(&chan->recvs);
                    while (item != opal_list_get_end(&chan->recvs)) {
                        next = opal_list_get_next(item);
                        req = (orcm_pnp_request_t*)item;
                        if (ORCM_PNP_TAG_WILDCARD == tag || tag == req->tag) {
                            opal_list_remove_item(&chan->recvs, item);
                            OBJ_RELEASE(item);
                        }
                        item = next;
                    }
                }
            }
            /* release the triplet */
            ORTE_RELEASE_THREAD(&triplet->ctl);
        }
        goto cleanup;
    }

    /* if this isn't either input or output channel, then get the channel object */
    if (NULL != (chan = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&channels, channel))) {
        ORCM_CREATE_STRING_ID(&string_id, app, version, release);
        item = opal_list_get_first(&chan->recvs);
        while (item != opal_list_get_end(&chan->recvs)) {
            next = opal_list_get_next(item);
            req = (orcm_pnp_request_t*)item;
            if (0 == strcasecmp(string_id, req->string_id)) {
                if (ORCM_PNP_TAG_WILDCARD == tag || tag == req->tag) {
                    opal_list_remove_item(&chan->recvs, item);
                    OBJ_RELEASE(item);
                }
            }
            item = next;
        }
        free(string_id);
    }

 cleanup:
    /* clear the thread */
    ORTE_RELEASE_THREAD(&local_thread);
    
    return ret;
}

static int default_output(orcm_pnp_channel_t channel,
                          orte_process_name_t *recipient,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg, int count,
                          opal_buffer_t *buffer)
{
    int i, ret;
    opal_buffer_t *buf;
    orcm_pnp_channel_t chan;
    
    /* if we have not announced, ignore this message */
    if (NULL == my_string_id) {
        return ORCM_ERR_NOT_AVAILABLE;
    }

    /* protect against threading */
    ORTE_ACQUIRE_THREAD(&local_thread);
    
    /* setup the message for xmission */
    if (ORTE_SUCCESS != (ret = construct_msg(&buf, buffer, tag, msg, count))) {
        ORTE_ERROR_LOG(ret);
        ORTE_RELEASE_THREAD(&local_thread);
        return ret;
    }
    
    /* if this is intended for everyone who might be listening on this channel,
     * multicast it to all groups in this channel
     */
    if (NULL == recipient ||
        (ORTE_JOBID_WILDCARD == recipient->jobid &&
         ORTE_VPID_WILDCARD == recipient->vpid)) {
        /* if this is going on the group channel, then substitute that channel here */
        if (ORCM_PNP_GROUP_OUTPUT_CHANNEL == channel) {
            chan = my_output_channel->channel;
        } else if (ORCM_PNP_GROUP_INPUT_CHANNEL == channel) {
            chan = my_input_channel->channel;
        } else {
            chan = channel;
        }
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:sending multicast of %d %s to channel %s tag %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (NULL == msg) ? (int)buffer->bytes_used : count,
                             (NULL == msg) ? "bytes" : "iovecs",
                             orcm_pnp_print_channel(chan),
                             orcm_pnp_print_tag(tag)));
        
        /* send the data to the channel */
        if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer(chan, tag, buf))) {
            ORTE_ERROR_LOG(ret);
        }
        OBJ_RELEASE(buf);
        ORTE_RELEASE_THREAD(&local_thread);
        return ORCM_SUCCESS;
    }
    
    /* if only one name field is WILDCARD, I don't know how to send
     * it - at least, not right now
     */
    if (ORTE_JOBID_WILDCARD == recipient->jobid ||
        ORTE_VPID_WILDCARD == recipient->vpid) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_IMPLEMENTED);
        OBJ_RELEASE(buf);
        ORTE_RELEASE_THREAD(&local_thread);
        return ORTE_ERR_NOT_IMPLEMENTED;
    }
    
    /* intended for a specific recipient, send it over p2p */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending p2p message of %d %s to tag %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == msg) ? (int)buffer->bytes_used : count,
                         (NULL == msg) ? "bytes" : "iovecs",
                         orcm_pnp_print_tag(tag)));

    /* release the thread prior to send */
    ORTE_RELEASE_THREAD(&local_thread);
    
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
                             opal_buffer_t *buffer,
                             orcm_pnp_callback_fn_t cbfunc,
                             void *cbdata)
{
    int i, ret;
    orcm_pnp_send_t *send;
    opal_buffer_t *buf;
    orcm_pnp_channel_t chan;

    /* if we have not announced, ignore this message */
    if (NULL == my_string_id) {
        return ORCM_ERR_NOT_AVAILABLE;
    }

    /* protect against threading */
    ORTE_ACQUIRE_THREAD(&local_thread);
    
    send = OBJ_NEW(orcm_pnp_send_t);
    send->tag = tag;
    send->msg = msg;
    send->count = count;
    send->buffer = buffer;
    send->cbfunc = cbfunc;
    send->cbdata = cbdata;

    /* setup the message for xmission */
    if (ORTE_SUCCESS != (ret = construct_msg(&buf, buffer, tag, msg, count))) {
        ORTE_ERROR_LOG(ret);
        ORTE_RELEASE_THREAD(&local_thread);
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
        if (ORCM_PNP_GROUP_OUTPUT_CHANNEL == channel) {
            chan = my_output_channel->channel;
        } else if (ORCM_PNP_GROUP_INPUT_CHANNEL == channel) {
            chan = my_input_channel->channel;
        } else {
            chan = channel;
        }
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:sending multicast of %d %s to channel %s tag %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (NULL == msg) ? (int)buffer->bytes_used : count,
                             (NULL == msg) ? "bytes" : "iovecs",
                             orcm_pnp_print_channel(channel),
                             orcm_pnp_print_tag(tag)));
        
        /* release thread prior to send */
        ORTE_RELEASE_THREAD(&local_thread);
        /* send the iovecs to the channel */
        if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer_nb(chan, tag, buf,
                                                              rmcast_callback, send))) {
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
        ORTE_RELEASE_THREAD(&local_thread);
        return ORTE_ERR_NOT_IMPLEMENTED;
    }
    
    /* intended for a specific recipient, send it over p2p */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending p2p message of %d %s to channel %s tag %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == msg) ? (int)buffer->bytes_used : count,
                         (NULL == msg) ? "bytes" : "iovecs",
                         orcm_pnp_print_channel(channel),
                         orcm_pnp_print_tag(tag)));

    /* release thread prior to send */
    ORTE_RELEASE_THREAD(&local_thread);
    
    /* send the msg */
    if (0 > (ret = orte_rml.send_buffer_nb(recipient, buf,
                                           ORTE_RML_TAG_MULTICAST_DIRECT, 0,
                                           rml_callback, send))) {
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

static char* get_string_id(void)
{
    if (NULL == my_string_id) {
        return NULL;
    }

    return strdup(my_string_id);
}

static int disable_comm(void)
{
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default: disabling comm",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));

    /* cancel the recvs, if active */
    if (recv_on) {
        orte_rmcast.cancel_recv(ORTE_RMCAST_WILDCARD_CHANNEL, ORTE_RMCAST_TAG_WILDCARD);
        orte_rml.recv_cancel(ORTE_NAME_WILDCARD, ORTE_RML_TAG_MULTICAST_DIRECT);
        recv_on = false;
    }
    
    /* stop the thread */
    if (orcm_pnp_base.use_threads) {
        msg_thread_end = true;
        opal_thread_join(&msg_thread, NULL);
    }
}


static int default_finalize(void)
{
    int i;
    orcm_triplet_t *triplet;
    orcm_pnp_channel_obj_t *chan;
    opal_list_item_t *item;

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default: finalizing",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    while (NULL != (item = opal_list_remove_first(&msg_delivery))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&msg_delivery);
    OBJ_DESTRUCT(&msg_ctl);

    /* destruct the threading support */
    OBJ_DESTRUCT(&local_thread);
    
    /* release the array of known channels */
    for (i=0; i < channels.size; i++) {
        if (NULL != (chan = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&channels, i))) {
            OBJ_RELEASE(chan);
        }
    }
    OBJ_DESTRUCT(&channels);

    return ORCM_SUCCESS;
}


/****    LOCAL  FUNCTIONS    ****/
static void recv_announcements(orte_process_name_t *sender,
                               opal_buffer_t *buf)
{
    opal_list_item_t *itm2;
    orcm_triplet_t *triplet;
    orcm_triplet_group_t *grp;
    orcm_source_t *source;
    char *app=NULL, *version=NULL, *release=NULL, *string_id=NULL, *nodename=NULL;
    orte_process_name_t originator;
    opal_buffer_t ann;
    int rc, n, i, j;
    orte_rmcast_channel_t input, output;
    orcm_pnp_send_t *pkt;
    orcm_pnp_channel_t chan;
    orte_job_t *daemons;
    orte_proc_t *proc;
    uint32_t uid;
    bool known=true;
    char *rml_uri=NULL;
    
    /* don't lock the thread here - it was previously locked */
    
    /* unpack the sender's triplet */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &string_id, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    /* get its input multicast channel */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &input, &n, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* get its output multicast channel */
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

    /* set the contact info - this has to be done even if the source is known
     * as it could be a repeat invocation of the same application
     */
    if (ORTE_SUCCESS != (rc = orte_rml.set_contact_info(rml_uri))) {
        ORTE_ERROR_LOG(rc);
        goto RELEASE;
    }

    /* who are they responding to? */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &originator, &n, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement from app %s channel %s on node %s uid %u",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         string_id, orcm_pnp_print_channel(output), nodename, uid));
    
    /* break the stringid into its elements */
    ORCM_DECOMPOSE_STRING_ID(string_id, app, version, release);

    /* find this triplet - create if not found */
    triplet = orcm_get_triplet(app, version, release, true);
    /* get the sender's group - create if not found */
    grp = orcm_get_triplet_group(triplet, sender->jobid, true);

    /* record the multicast channels it is on */
    grp->input = input;
    grp->output = output;
    /* check for any pending recvs */
    check_pending_recvs(triplet, grp);
    /* notify the user, if requested */
    if (NULL != grp->pnp_cbfunc) {
        /* if the user requested a callback, they probably intend to send
         * something to this triplet - so ensure the channel to its input is open
         */
        if (ORTE_SUCCESS != (rc = orte_rmcast.open_channel(input, string_id, NULL, -1, NULL, ORTE_RMCAST_XMIT))) {
            ORTE_ERROR_LOG(rc);
            ORTE_RELEASE_THREAD(&triplet->ctl);
            goto RELEASE;
        }
        grp->pnp_cbfunc(app, version, release, input);
        /* flag that the callback for this jobid/grp has been done */
        grp->pnp_cb_done = true;
        grp->pnp_cbfunc = NULL;
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement from source %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));
    
    /* get the source object - do not create if not found */
    source = orcm_get_source_in_group(grp, sender->vpid, false);
    if (NULL == source) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received adding source %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(sender)));
        
        source = OBJ_NEW(orcm_source_t);
        source->name.jobid = sender->jobid;
        source->name.vpid = sender->vpid;
        /* constructor sets alive to false */
        source->alive = true;
        opal_pointer_array_set_item(&grp->members, sender->vpid, source);
        known = false;
    } else {
        /* update its name in case it isn't known yet */
        source->name.jobid = sender->jobid;
        source->name.vpid = sender->vpid;
        /* flag it as alive */
        source->alive = true;
        /* the source returns locked, so release it */
        ORTE_RELEASE_THREAD(&source->ctl);
    }
    /* release the triplet thread */
    ORTE_RELEASE_THREAD(&triplet->ctl);

    /* if this is a new source and they wanted a callback,
     * now is the time to do it in case it needs to do some
     * prep before we can send our return announcement
     */
    if (!known && NULL != my_announce_cbfunc) {
        /* have to release the local thread in case this function does something right away */
        ORTE_RELEASE_THREAD(&local_thread);
        my_announce_cbfunc(app, version, release, sender, nodename, rml_uri, uid);
        /* need to reacquire the thread to avoid double-release */
        ORTE_ACQUIRE_THREAD(&local_thread);
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default: announcement sent in response to originator %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(&originator)));
    
    /* if they were responding to an announcement by someone,
     * then don't respond or else we'll go into an infinite
     * loop of announcements
     */
    if (originator.jobid != ORTE_JOBID_INVALID &&
        originator.vpid != ORTE_VPID_INVALID) {
        /* nothing more to do */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:recvd_ann response to another announce - ignoring",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
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
    if (ORCM_SUCCESS != (rc = pack_announcement(&ann, sender))) {
        if (ORCM_ERR_NOT_AVAILABLE != rc) {
            /* not-avail => have not announced ourselves yet */
            ORTE_ERROR_LOG(rc);
        }
        OBJ_DESTRUCT(&ann);
        goto RELEASE;
    }
    
    /* send it - have to release the thread in case we receive something right away */
    ORTE_RELEASE_THREAD(&local_thread);
    if (ORCM_SUCCESS != (rc = default_output(chan, NULL,
                                             ORCM_PNP_TAG_ANNOUNCE,
                                             NULL, 0, &ann))) {
        ORTE_ERROR_LOG(rc);
    }
    /* cleanup */
    OBJ_DESTRUCT(&ann);
    /* do not re-release the thread */
    goto cleanup;

 RELEASE:
    ORTE_RELEASE_THREAD(&local_thread);
    
 cleanup:
    if (NULL != app) {
        free(app);
    }
    if (NULL != version) {
        free(version);
    }
    if (NULL != release) {
        free(release);
    }
    if (NULL != string_id) {
        free(string_id);
    }
    if (NULL != rml_uri) {
        free(rml_uri);
    }
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:recvd_announce complete",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    return;
}

static void recv_input_buffers(int status,
                               orte_rmcast_channel_t channel,
                               orte_rmcast_seq_t seq_num,
                               orte_rmcast_tag_t tag,
                               orte_process_name_t *sender,
                               opal_buffer_t *buf, void *cbdata)
{
    orcm_pnp_request_t *request;
    int rc, n;
    char *string_id=NULL;
    orcm_pnp_tag_t tg;
    orcm_pnp_channel_obj_t *chan;
    orte_jobid_t jobid;
    orcm_pnp_msg_t *msg;
    int8_t flag;

    /* if we have not announced, ignore this message */
    if (NULL == my_string_id) {
        opal_output(0, "%s NO STRING ID", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        return;
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received input buffer on channel %s seq %lu tag %s from sender %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), orcm_pnp_print_channel(channel), seq_num,
                         orcm_pnp_print_tag(tag), ORTE_NAME_PRINT(sender)));
    
    /* if this message is from myself, ignore it */
    if (sender->jobid == ORTE_PROC_MY_NAME->jobid &&
        sender->vpid == ORTE_PROC_MY_NAME->vpid) {
        return;
    }

    /* protect against threading */
    ORTE_ACQUIRE_THREAD(&local_thread);
    
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

    /* if this is an announcement, process it immediately - do not
     * push it onto the recv thread! Otherwise, any immediate msgs
     * sent by that proc can be lost due to a race condition
     */
    if (ORCM_PNP_TAG_ANNOUNCE == tg) {
        free(string_id);
        /* unpack the iovec vs buffer flag to maintain place in buffer */
        n=1;
        if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &flag, &n, OPAL_INT8))) {
            ORTE_ERROR_LOG(rc);
            goto DEPART;
        }
        /* the recv_announcements fn will release the thread */
        recv_announcements(sender, buf);
        return;
    }

    /* check to see if the message is from the leader of this triplet */
    if (!orcm_leader.deliver_msg(string_id, sender, channel, seq_num)) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s Message from %s of triplet %s ignored - not leader",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(sender), string_id));
        goto DEPART;
    }

    /* get the channel object */
    if (NULL == (chan = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&channels, channel))) {
        /* unrecognized channel - ignore message */
        goto DEPART;
    }

    /* find the request object for this tag */
    if (NULL == (request = find_request(&chan->recvs, string_id, tg))) {
        /* no matching requests */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:recv triplet %s has no matching recvs for channel %s tag %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             string_id, orcm_pnp_print_channel(channel), orcm_pnp_print_tag(tg)));
        goto DEPART;
    }

    /* push this message onto the delivery list */
    msg = OBJ_NEW(orcm_pnp_msg_t);
    msg->sender.jobid = sender->jobid;
    msg->sender.vpid = sender->vpid;
    msg->tag = tag;
    opal_dss.copy_payload(&msg->buf, buf);
    msg->cbfunc = request->cbfunc;
    msg->cbdata = request->cbdata;

    if (orcm_pnp_base.use_threads) {
        ORTE_ACQUIRE_THREAD(&msg_ctl);
        opal_list_append(&msg_delivery, &msg->super);
        /* release our thread in case this gets processed immediately */
        ORTE_RELEASE_THREAD(&local_thread);
        ORTE_RELEASE_THREAD(&msg_ctl);
    } else {
        ORTE_RELEASE_THREAD(&local_thread);
        process_msg(msg);
    }
    return;

 DEPART:
    if (NULL != string_id) {
        free(string_id);
    }
    /* release the thread */
    ORTE_RELEASE_THREAD(&local_thread);
}

/* pack the common elements of an announcement message */
static int pack_announcement(opal_buffer_t *buf, orte_process_name_t *sender)
{
    int ret;
    char *rml_uri;
    orcm_pnp_channel_t chan;

    /* if we haven't registered an app-triplet yet, then we can't announce */
    if (NULL == my_string_id) {
        return ORCM_ERR_NOT_AVAILABLE;
    }
    
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_string_id, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    /* pack my input channel */
    if (NULL != my_input_channel) {
        chan = my_input_channel->channel;
    } else {
        chan = ORCM_PNP_INVALID_CHANNEL;
    }
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &chan, 1, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }

    /* pack my output channel */
    if (NULL != my_output_channel) {
        chan = my_output_channel->channel;
    } else {
        chan = ORCM_PNP_INVALID_CHANNEL;
    }
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &chan, 1, ORTE_RMCAST_CHANNEL_T))) {
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
static void rmcast_callback(int status,
                            orte_rmcast_channel_t channel,
                            orte_rmcast_seq_t seq_num,
                            orte_rmcast_tag_t tag,
                            orte_process_name_t *sender,
                            opal_buffer_t *buf, void* cbdata)
{
    orcm_pnp_send_t *send = (orcm_pnp_send_t*)cbdata;
    
    if (NULL != send->cbfunc) {
        send->cbfunc(status, ORTE_PROC_MY_NAME, send->tag, send->msg, send->count, send->buffer, send->cbdata);
    }
    OBJ_RELEASE(send);
}

static void rml_callback(int status,
                         orte_process_name_t* sender,
                         opal_buffer_t* buffer,
                         orte_rml_tag_t tag,
                         void* cbdata)
{
    orcm_pnp_send_t *send = (orcm_pnp_send_t*)cbdata;
    
    /* do any required callbacks */
    if (NULL != send->cbfunc) {
        send->cbfunc(status, ORTE_PROC_MY_NAME, send->tag, send->msg, send->count, send->buffer, send->cbdata);
    }
    OBJ_RELEASE(send);
}

static void recv_direct_msgs(int status, orte_process_name_t* sender,
                             opal_buffer_t* buffer, orte_rml_tag_t tg,
                             void* cbdata)
{
    int rc;
    int32_t n, i, sz, iovec_count;
    orcm_pnp_tag_t tag;
    struct iovec *iovec_array;
    orcm_pnp_request_t *request;
    char *string_id;
    orcm_pnp_msg_t *msg;

    /* if we have not announced, ignore this message */
    if (NULL == my_string_id) {
        return;
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default recvd direct msg from %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));
    
    /* protect against threading */
    ORTE_ACQUIRE_THREAD(&local_thread);
    
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
    
    /* find the request object for this tag */
    if (NULL == (request = find_request(&my_input_channel->recvs, string_id, tag))) {
        opal_output(0, "%s A direct message was received, but no matching recv"
                    " has been posted for direct messages sent from trip %s"
                    " tag %s or to the WILDCARD tag",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), string_id, orcm_pnp_print_tag(tag));
        goto CLEANUP;
    } else {
        /* push this message onto the delivery list */
        msg = OBJ_NEW(orcm_pnp_msg_t);
        msg->sender.jobid = sender->jobid;
        msg->sender.vpid = sender->vpid;
        msg->tag = tag;
        opal_dss.copy_payload(&msg->buf, buffer);
        msg->cbfunc = request->cbfunc;

        if (orcm_pnp_base.use_threads) {
            ORTE_ACQUIRE_THREAD(&msg_ctl);
            opal_list_append(&msg_delivery, &msg->super);
            /* release our thread in case this gets processed immediately */
            ORTE_RELEASE_THREAD(&local_thread);
            ORTE_RELEASE_THREAD(&msg_ctl);
        } else {
            ORTE_RELEASE_THREAD(&local_thread);
            process_msg(msg);
        }
        goto DEPART;
    }
   
 CLEANUP:
    /* release the thread */
    ORTE_RELEASE_THREAD(&local_thread);
    
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

static orcm_pnp_request_t* find_request(opal_list_t *list,
                                        char *string_id,
                                        orcm_pnp_tag_t tag)
{
    orcm_pnp_request_t *req;
    opal_list_item_t *item;

    for (item = opal_list_get_first(list);
         item != opal_list_get_end(list);
         item = opal_list_get_next(item)) {
        req = (orcm_pnp_request_t*)item;

        /* check the tag first */
        if (tag != req->tag && ORCM_PNP_TAG_WILDCARD != req->tag) {
            continue;
        }
        /* tags match - check the string_id's */
        if (orcm_triplet_cmp(string_id, req->string_id)) {
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s MATCHED RECV %s TO %s TAG %s:%s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), string_id,
                                 req->string_id, orcm_pnp_print_tag(tag),
                                 orcm_pnp_print_tag(req->tag)));
            return req;
        }
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

/* given a triplet_group, check logged recvs to see if any need to be moved
 * to the channel array for the group's channels. This includes looking at
 * recvs placed against the specified triplet, AND recvs placed against
 * triplets that include wildcards spanning the provided grp
 */
static void check_trip_recvs(char *stringid,
                             opal_list_t *recvs,
                             orcm_pnp_channel_t channel)
{
    opal_list_item_t *item;
    orcm_pnp_request_t *req, *reqcp;
    orcm_pnp_channel_obj_t *recvr=NULL;
    int rc;

    if (0 < opal_list_get_size(recvs) && ORCM_PNP_INVALID_CHANNEL != channel) {
        /* create a copy of any recvs on the appropriate
         * channel in the channel array, avoiding duplication
         * since this function could be called multiple times
         */
        for (item = opal_list_get_first(recvs);
             item != opal_list_get_end(recvs);
             item = opal_list_get_next(item)) {
            req = (orcm_pnp_request_t*)item;

            /* get the associated channel object */
            if (NULL == recvr) {
                if (NULL == (recvr = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&channels, channel))) {
                    recvr = OBJ_NEW(orcm_pnp_channel_obj_t);
                    recvr->channel = channel;
                    opal_pointer_array_set_item(&channels, recvr->channel, recvr);
                }
            }
            /* see if this recv is already present */
            if (NULL == (reqcp = find_request(&recvr->recvs, req->string_id, req->tag))) {
                /* not already present - create it */
                reqcp = OBJ_NEW(orcm_pnp_request_t);
                reqcp->string_id = strdup(req->string_id);
                reqcp->tag = req->tag;
                opal_list_append(&recvr->recvs, &reqcp->super);
            }
            /* SPECIAL CASE: if the string_id matches my own and the
             * recvr channel matches my input channel, then this recv
             * is for messages sent to me. Since anyone can do so, set
             * the string_id in this case to WILDCARD
             */
            if (NULL != my_string_id &&
                recvr == my_input_channel &&
                0 == strcasecmp(my_string_id, req->string_id)) {
                free(reqcp->string_id);
                reqcp->string_id = strdup(ORCM_WILDCARD_STRING_ID);
            }
            /* update the cbfunc */
            reqcp->cbfunc = req->cbfunc;
            
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s pnp:default:check_trip_recvs putting recv for %s:%s on channel %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), reqcp->string_id,
                                 orcm_pnp_print_tag(reqcp->tag),
                                 orcm_pnp_print_channel(recvr->channel)));
        }
        /* if we have any recvs now, ensure the channel is open */
        if (NULL != recvr && 0 < opal_list_get_size(&recvr->recvs)) {
            /* open the channel */
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s pnp:default:check_pending_recvs setup recv for channel %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), orcm_pnp_print_channel(recvr->channel)));
            if (ORTE_SUCCESS != (rc = orte_rmcast.open_channel(recvr->channel, stringid,
                                                               NULL, -1, NULL, ORTE_RMCAST_RECV))) {
                ORTE_ERROR_LOG(rc);
            }
            /* setup the recv */
            if (ORTE_SUCCESS != (rc = orte_rmcast.recv_buffer_nb(recvr->channel,
                                                                 ORTE_RMCAST_TAG_WILDCARD,
                                                                 ORTE_RMCAST_PERSISTENT,
                                                                 recv_input_buffers, NULL))) {
                ORTE_ERROR_LOG(rc);
            }
        }
    }
}

static void check_pending_recvs(orcm_triplet_t *trp,
                                orcm_triplet_group_t *grp)
{
    int i;
    orcm_triplet_t *triplet;

    /* check the wildcard triplets and see if any match */
    /* lock the global triplet arrays for our use */
    ORTE_ACQUIRE_THREAD(&orcm_triplets->ctl);
    for (i=0; i < orcm_triplets->wildcards.size; i++) {
        if (NULL == (triplet = (orcm_triplet_t*)opal_pointer_array_get_item(&orcm_triplets->wildcards, i))) {
            continue;
        }
        if (orcm_triplet_cmp(trp->string_id, triplet->string_id)) {
            /* match found - copy the recvs to the appropriate list */
            if (ORCM_PNP_INVALID_CHANNEL != grp->input) {
                check_trip_recvs(triplet->string_id, &triplet->input_recvs, grp->input);
            }
            if (ORCM_PNP_INVALID_CHANNEL != grp->output) {
                check_trip_recvs(triplet->string_id, &triplet->output_recvs, grp->output);
            }
            /* copy notification policies, if not already set */
            if (ORCM_NOTIFY_NONE == trp->notify) {
                trp->notify = triplet->notify;
                trp->leader_cbfunc = triplet->leader_cbfunc;
            }
        }
    }
    /* release the global arrays */
    ORTE_RELEASE_THREAD(&orcm_triplets->ctl);
           

    /* check the triplet input_recv list */
    if (ORCM_PNP_INVALID_CHANNEL != grp->input) {
        check_trip_recvs(trp->string_id, &trp->input_recvs, grp->input);
    }

    /* check the triplet output_recv list */
    if (ORCM_PNP_INVALID_CHANNEL != grp->output) {
        check_trip_recvs(trp->string_id, &trp->output_recvs, grp->output);
    }

    /* if we haven't already done a callback on this group, check the callback policy */
    if (!grp->pnp_cb_done) {
        if (ORTE_JOBID_WILDCARD == trp->pnp_cb_policy) {
            /* get a callback from every jobid */
            grp->pnp_cbfunc = trp->pnp_cbfunc;
        } else if (ORTE_JOBID_INVALID == trp->pnp_cb_policy ||
                   grp->jobid == trp->pnp_cb_policy) {
            grp->pnp_cbfunc = trp->pnp_cbfunc;
            /* only one jobid generates a callback */
            trp->pnp_cb_policy = ORTE_JOBID_MAX;
        }
    }
}

/* cycle through all the groups in a triplet and update the channel
 * array if triplet recvs apply
 */
static void update_pending_recvs(orcm_triplet_t *trp)
{
    orcm_triplet_group_t *grp;
    int i;

    for (i=0; i < trp->groups.size; i++) {
        if (NULL == (grp = (orcm_triplet_group_t*)opal_pointer_array_get_item(&trp->groups, i))) {
            continue;
        }
        check_trip_recvs(trp->string_id, &trp->input_recvs, grp->input);
        if (grp->input != grp->output) {
            check_trip_recvs(trp->string_id, &trp->output_recvs, grp->output);
        }
    }
}

static int record_recv(orcm_triplet_t *triplet,
                       orcm_pnp_channel_t channel,
                       orcm_pnp_tag_t tag,
                       orcm_pnp_callback_fn_t cbfunc,
                       void *cbdata)
{
    orcm_pnp_request_t *req;
    orcm_pnp_channel_obj_t *chan;
    opal_list_item_t *item;
    int ret = ORCM_SUCCESS;

    if (ORCM_PNP_GROUP_INPUT_CHANNEL == channel) {
        /* SPECIAL CASE: if this is my triplet, then I am asking for all input
         * sent to me. Since anyone can send to me, substitute the
         * wildcard string id
         */
        if (triplet == my_triplet) {
            if (NULL != find_request(&triplet->input_recvs, ORCM_WILDCARD_STRING_ID, tag)) {
                /* already exists - nothing to do */
                goto cleanup;
            }
            /* create it */
            req = OBJ_NEW(orcm_pnp_request_t);
            req->string_id = strdup(ORCM_WILDCARD_STRING_ID);
            req->tag = tag;
            req->cbfunc = cbfunc;
            req->cbdata = cbdata;
            opal_list_append(&triplet->input_recvs, &req->super);
        } else {
            /* want the input from another triplet */
            if (NULL != find_request(&triplet->input_recvs, triplet->string_id, tag)) {
                /* already exists - nothing to do */
                goto cleanup;
            }
            /* create it */
            req = OBJ_NEW(orcm_pnp_request_t);
            req->string_id = strdup(triplet->string_id);
            req->tag = tag;
            req->cbfunc = cbfunc;
            req->cbdata = cbdata;
            opal_list_append(&triplet->input_recvs, &req->super);
        }
        /* update channel recv info for all triplet-groups already known */
        update_pending_recvs(triplet);
        goto cleanup;
    } else if (ORCM_PNP_GROUP_OUTPUT_CHANNEL == channel) {
        /* see if the request already exists on the triplet's output queue */
        if (NULL != find_request(&triplet->output_recvs, triplet->string_id, tag)) {
            /* already exists - nothing to do */
            goto cleanup;
        }
        /* create it */
        req = OBJ_NEW(orcm_pnp_request_t);
        req->string_id = strdup(triplet->string_id);
        req->tag = tag;
        req->cbfunc = cbfunc;
        req->cbdata = cbdata;
        opal_list_append(&triplet->output_recvs, &req->super);
        /* update channel recv info for all triplet-groups already known */
        update_pending_recvs(triplet);
        goto cleanup;
    } else {
        /* get the specified channel */
        if (NULL == (chan = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&channels, channel))) {
            chan = OBJ_NEW(orcm_pnp_channel_obj_t);
            chan->channel = channel;
            opal_pointer_array_set_item(&channels, chan->channel, chan);
        }
        /* see if this req already exists */
        for (item = opal_list_get_first(&chan->recvs);
             item != opal_list_get_end(&chan->recvs);
             item = opal_list_get_next(item)) {
            req = (orcm_pnp_request_t*)item;

            /* check the tag first */
            if (tag != req->tag) {
                continue;
            }
            /* tags match - check the string_id's */
            if (0 != strcasecmp(triplet->string_id, req->string_id)) {
                continue;
            }
            /* we have an exact match - update the cbfunc */
            req->cbfunc = cbfunc;
            goto proceed;
        }
        /* if we get here, then no exact match was found, so create a new entry */
        req = OBJ_NEW(orcm_pnp_request_t);
        req->string_id = strdup(triplet->string_id);
        req->tag = tag;
        req->cbfunc = cbfunc;
        req->cbdata = cbdata;
        opal_list_append(&chan->recvs, &req->super);
    }

 proceed:
    if (chan->channel < ORCM_PNP_SYS_CHANNEL) {
        /* can't register rmcast recvs on group_input, group_output, and wildcard channels */
        goto cleanup;
    }

    /* open this channel - will just return if already open */
    if (ORCM_SUCCESS != (ret = orte_rmcast.open_channel(chan->channel, triplet->string_id,
                                                        NULL, -1, NULL,
                                                        ORTE_RMCAST_RECV))) {
        if (ORTE_EXISTS != ret) {
            ORTE_ERROR_LOG(ret);
            goto cleanup;
        }
    }
    /* setup to listen to it - will just return if we already are */
    if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(chan->channel, ORTE_RMCAST_TAG_WILDCARD,
                                                          ORTE_RMCAST_PERSISTENT,
                                                          recv_input_buffers, NULL))) {
        if (ORTE_EXISTS == ret) {
            ret = ORTE_SUCCESS;
            goto cleanup;
        }
        ORTE_ERROR_LOG(ret);
    }

 cleanup:
    return ret;
}

static void process_msg(orcm_pnp_msg_t *msg)
{
    int n, rc;
    int8_t flag;
    int32_t i, num_iovecs, num_bytes;
    struct iovec *iovecs=NULL;

    /* unpack the iovec vs buffer flag */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(&msg->buf, &flag, &n, OPAL_INT8))) {
        ORTE_ERROR_LOG(rc);
        goto DEPART;
    }    
    
    if (0 == flag) {
        /* iovecs were sent - get them */
        n=1;
        if (ORCM_SUCCESS != (rc = opal_dss.unpack(&msg->buf, &num_iovecs, &n, OPAL_INT32))) {
            ORTE_ERROR_LOG(rc);
            goto DEPART;
        }
        if (0 < num_iovecs) {
            iovecs = (struct iovec *)malloc(num_iovecs * sizeof(struct iovec));
            for (i=0; i < num_iovecs; i++) {
                n=1;
                if (ORCM_SUCCESS != (rc = opal_dss.unpack(&msg->buf, &num_bytes, &n, OPAL_INT32))) {
                    ORTE_ERROR_LOG(rc);
                    goto DEPART;
                }
                iovecs[i].iov_len = num_bytes;
                if (0 < num_bytes) {
                    iovecs[i].iov_base = (uint8_t*)malloc(num_bytes);
                    if (ORCM_SUCCESS != (rc = opal_dss.unpack(&msg->buf, iovecs[i].iov_base, &num_bytes, OPAL_UINT8))) {
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

        /* release the local thread prior to executing the callback
         * to avoid deadlock
         */
        if (orcm_pnp_base.use_threads) {
            ORTE_RELEASE_THREAD(&msg_ctl);
            if (msg_thread_end) {
                /* make one last check - if comm is disabled, don't deliver msg */
                OBJ_RELEASE(msg);
                return;
            }
        }
        msg->cbfunc(ORCM_SUCCESS, &msg->sender, msg->tag, iovecs, num_iovecs, NULL, msg->cbdata);
        if (orcm_pnp_base.use_threads) {
            ORTE_ACQUIRE_THREAD(&msg_ctl);
        }
        /* release the memory */
        if (0 < num_iovecs) {
            for (i=0; i < num_iovecs; i++) {
                free(iovecs[i].iov_base);
            }
            free(iovecs);
        }
        goto DEPART;
    }

    if (1 == flag) {
        /* buffer was sent - just hand it over */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received input buffer - delivering msg",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        /* release the thread prior to executing the callback
         * to avoid deadlock
         */
        if (orcm_pnp_base.use_threads) {
            ORTE_RELEASE_THREAD(&msg_ctl);
            if (msg_thread_end) {
                /* make one last check - if comm is disabled, don't deliver msg */
                OBJ_RELEASE(msg);
                return;
            }
        }
        msg->cbfunc(ORCM_SUCCESS, &msg->sender, msg->tag, NULL, 0, &msg->buf, msg->cbdata);
        if (orcm_pnp_base.use_threads) {
            ORTE_ACQUIRE_THREAD(&msg_ctl);
        }
    }
 DEPART:
    OBJ_RELEASE(msg);
}

static void* deliver_msg(opal_object_t *obj)
{
    orcm_pnp_msg_t *msg;
    struct timespec s;

    s.tv_sec = 1;
    s.tv_nsec = 10000L;

    while (1) {
        ORTE_ACQUIRE_THREAD(&msg_ctl);
        if (msg_thread_end) {
            ORTE_RELEASE_THREAD(&msg_ctl);
            return OPAL_THREAD_CANCELLED;
        }

        if (NULL == (msg = (orcm_pnp_msg_t*)opal_list_remove_first(&msg_delivery))) {
            ORTE_RELEASE_THREAD(&msg_ctl);
            /* don't hammer the cpu */
            nanosleep(&s, NULL);
            continue;
        }

        ORTE_RELEASE_THREAD(&msg_ctl);
    }

    return OPAL_THREAD_CANCELLED;
}

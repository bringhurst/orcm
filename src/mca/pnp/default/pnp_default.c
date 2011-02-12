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
#include "opal/threads/threads.h"
#include "opal/mca/sysinfo/sysinfo.h"

#include "orte/mca/rmcast/rmcast.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/routed/routed.h"
#include "orte/runtime/orte_globals.h"

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


/* Local variables */
static bool recv_on = false;

/* local thread support */
static orte_thread_ctl_t local_thread;

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

    /* record my channels */
    if (ORTE_SUCCESS != (ret = orte_rmcast.query_channel(&output, &input))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    
    if (ORCM_PNP_INVALID_CHANNEL != output) {
        orcm_pnp_base.my_output_channel = OBJ_NEW(orcm_pnp_channel_obj_t);
        orcm_pnp_base.my_output_channel->channel = output;
        opal_pointer_array_set_item(&orcm_pnp_base.channels, output, orcm_pnp_base.my_output_channel);
    }

    if (ORCM_PNP_INVALID_CHANNEL != input && output != input) {
        orcm_pnp_base.my_input_channel = OBJ_NEW(orcm_pnp_channel_obj_t);
        orcm_pnp_base.my_input_channel->channel = input;
        opal_pointer_array_set_item(&orcm_pnp_base.channels, input, orcm_pnp_base.my_input_channel);
    }

    /* record my uid */
    orcm_pnp_base.my_uid = (uint32_t)getuid();

    /* open the default channel */
    if (ORCM_PROC_IS_APP) {
        channel = ORTE_RMCAST_APP_PUBLIC_CHANNEL;
        stringid = "ORCM:APP:PUBLIC";
    } else {
        channel = ORTE_RMCAST_SYS_CHANNEL;
        stringid = "ORCM:DVM:SYSTEM";
    }

    if (NULL != orcm_pnp_base.my_input_channel &&
        orcm_pnp_base.my_input_channel->channel == channel) {
        chan = orcm_pnp_base.my_input_channel;
    } else if (NULL != orcm_pnp_base.my_output_channel &&
               orcm_pnp_base.my_output_channel->channel == channel) {
        chan = orcm_pnp_base.my_output_channel;
        /* for tools and system utilities, no input channel is assigned,
         * so set their input to be their output so direct messages sent
         * to them have a place to go
         */
        if (NULL == orcm_pnp_base.my_input_channel) {
            orcm_pnp_base.my_input_channel = orcm_pnp_base.my_output_channel;
        }
    } else {
        chan = OBJ_NEW(orcm_pnp_channel_obj_t);
        chan->channel = channel;
        opal_pointer_array_set_item(&orcm_pnp_base.channels, channel, chan);
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

    /* start the processing thread */
    if (ORTE_SUCCESS != (ret = orcm_pnp_base_start_threads())) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }

    /* setup to listen to it - will just return if we already are */
    if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(chan->channel,
                                                          ORTE_RMCAST_TAG_WILDCARD,
                                                          ORTE_RMCAST_PERSISTENT,
                                                          orcm_pnp_base_recv_input_buffers, NULL))) {
        if (ORTE_EXISTS != ret) {
            ORTE_ERROR_LOG(ret);
            return ret;
        }
    }

    /* setup an RML recv to catch any direct messages */
    if (ORTE_SUCCESS != (ret = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD,
                                                       ORTE_RML_TAG_MULTICAST_DIRECT,
                                                       ORTE_RML_PERSISTENT,
                                                       orcm_pnp_base_recv_direct_msgs,
                                                       NULL))) {
        if (ORTE_EXISTS != ret) {
            ORTE_ERROR_LOG(ret);
            return ret;
        }
    }

    orcm_pnp_base.comm_enabled = true;
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
    
    if (!orcm_pnp_base.comm_enabled) {
        return ORCM_ERR_COMM_DISABLED;
    }

    /* protect against threading */
    ORTE_ACQUIRE_THREAD(&local_thread);
    
    if (NULL != orcm_pnp_base.my_string_id) {
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
    ORCM_CREATE_STRING_ID(&orcm_pnp_base.my_string_id, app, version, release);
    
    /* retain the callback function */
    orcm_pnp_base.my_announce_cbfunc = cbfunc;
    
    /* get a triplet object for myself - creates
     * it if one doesn't already exist
     */
    orcm_pnp_base.my_triplet = orcm_get_triplet(app, version, release, true);
    /* get my group object */
    orcm_pnp_base.my_group = orcm_get_triplet_group(orcm_pnp_base.my_triplet, ORTE_PROC_MY_NAME->jobid, true);
    orcm_pnp_base.my_group->uid = orcm_pnp_base.my_uid;
    orcm_pnp_base.my_group->input = orcm_pnp_base.my_input_channel->channel;
    orcm_pnp_base.my_group->output = orcm_pnp_base.my_output_channel->channel;

    /* check for pending recvs for these channels - this will copy
     * recvs that were pre-posted on the triplet to the channel
     * array
     */
    orcm_pnp_base_check_pending_recvs(orcm_pnp_base.my_triplet,
                                      orcm_pnp_base.my_group);

    /* release the triplet as we no longer require it */
    ORTE_RELEASE_THREAD(&orcm_pnp_base.my_triplet->ctl);

    /* no need to hold the lock any further */
    ORTE_RELEASE_THREAD(&local_thread);
    
    /* assemble the announcement message */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    
    /* pack the common elements */
    if (ORCM_SUCCESS != (ret = orcm_pnp_base_pack_announcement(&buf, ORTE_NAME_INVALID))) {
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
                         "%s pnp:default:open_channel for %s:%s:%s job %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL": release,
                         ORTE_JOBID_PRINT(jobid)));

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
                /* if the user requested a callback, they probably intend to send
                 * something to this triplet - so ensure the channel to its input is open.
                 * No need to release threads first as this call cannot result in callbacks
                 */
                if (ORTE_SUCCESS != (rc = orte_rmcast.open_channel(grp->input, triplet->string_id, NULL, -1, NULL, ORTE_RMCAST_XMIT))) {
                    ORTE_ERROR_LOG(rc);
                    ORTE_RELEASE_THREAD(&triplet->ctl);
                    continue;
                }
                /* release the threads before doing the callback in
                 * case the caller sends messages
                 */
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
        /* see if we have know about any group with this triplet */
        done = false;
        for (i=0; i < triplet->groups.size; i++) {
            if (NULL == (grp = (orcm_triplet_group_t*)opal_pointer_array_get_item(&triplet->groups, i))) {
                continue;
            }
            grp->pnp_cbfunc = cbfunc;
            if (ORCM_PNP_INVALID_CHANNEL != grp->input) {
                /* if the user requested a callback, they probably intend to send
                 * something to this triplet - so ensure the channel to its input is open.
                 * No need to release threads first as this call cannot result in callbacks
                 */
                if (ORTE_SUCCESS != (rc = orte_rmcast.open_channel(grp->input, triplet->string_id, NULL, -1, NULL, ORTE_RMCAST_XMIT))) {
                    ORTE_ERROR_LOG(rc);
                    continue;
                }
                 /* flag that we already did the callback so we don't do it again */
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
                /* if the user requested a callback, they probably intend to send
                 * something to this triplet - so ensure the channel to its input is open.
                 * No need to release threads first as this call cannot result in callbacks
                 */
                if (ORTE_SUCCESS != (rc = orte_rmcast.open_channel(grp->input, triplet->string_id, NULL, -1, NULL, ORTE_RMCAST_XMIT))) {
                    ORTE_ERROR_LOG(rc);
                    continue;
                }
                 /* release the threads before doing the callback in
                 * case the caller sends messages
                 */
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
        opal_pointer_array_add(&triplet->groups, grp);
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
                if (NULL == orcm_pnp_base_find_request(&triplet->input_recvs, triplet->string_id, tag)) {
                    /* create it */
                    req = OBJ_NEW(orcm_pnp_request_t);
                    req->string_id = strdup(triplet->string_id);
                    req->tag = tag;
                    req->cbfunc = cbfunc;
                    req->cbdata = cbdata;
                    opal_list_append(&triplet->input_recvs, &req->super);
                }
            } else {
                if (NULL == orcm_pnp_base_find_request(&triplet->output_recvs, triplet->string_id, tag)) {
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
                if (trp == triplet) {
                    /* don't copy from ourselves */
                    continue;
                }
                /* lock the triplet thread */
                ORTE_ACQUIRE_THREAD(&trp->ctl);
                if (orcm_triplet_cmp(trp->string_id, triplet->string_id)) {
                    /* triplet matches - transfer the recv */
                    if (ORCM_SUCCESS != (ret = orcm_pnp_base_record_recv(trp, channel, tag, cbfunc, cbdata))) {
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
            if (NULL == (recvr = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&orcm_pnp_base.channels, channel))) {
                recvr = OBJ_NEW(orcm_pnp_channel_obj_t);
                recvr->channel = channel;
                opal_pointer_array_set_item(&orcm_pnp_base.channels, recvr->channel, recvr);
            }
            if (NULL == (req = orcm_pnp_base_find_request(&recvr->recvs, triplet->string_id, tag))) {
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
                                                                  orcm_pnp_base_recv_input_buffers, NULL))) {
                if (ORTE_EXISTS == ret) {
                    ret = ORTE_SUCCESS;
                    goto cleanup;
                }
                ORTE_ERROR_LOG(ret);
            }
        }

    } else {
        /* we are dealing with a non-wildcard triplet - record the request */
        if (ORCM_SUCCESS != (ret = orcm_pnp_base_record_recv(triplet, channel, tag, cbfunc, cbdata))) {
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
        for (i=0; i < orcm_pnp_base.channels.size; i++) {
            if (NULL == (chan = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&orcm_pnp_base.channels, i))) {
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
                    chan = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&orcm_pnp_base.channels, grp->input);
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
                    chan = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&orcm_pnp_base.channels, grp->output);
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
    if (NULL != (chan = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&orcm_pnp_base.channels, channel))) {
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
    if (NULL == orcm_pnp_base.my_string_id) {
        return ORCM_ERR_NOT_AVAILABLE;
    }

    if (!orcm_pnp_base.comm_enabled) {
        return ORCM_ERR_COMM_DISABLED;
    }

    /* protect against threading */
    ORTE_ACQUIRE_THREAD(&local_thread);

    /* setup the message for xmission */
    if (ORTE_SUCCESS != (ret = orcm_pnp_base_construct_msg(&buf, buffer, tag, msg, count))) {
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
            chan = orcm_pnp_base.my_output_channel->channel;
        } else if (ORCM_PNP_GROUP_INPUT_CHANNEL == channel) {
            chan = orcm_pnp_base.my_input_channel->channel;
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
        return ret;
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
                         "%s pnp:default:sending p2p message of %d %s to %s tag %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == msg) ? (int)buffer->bytes_used : count,
                         (NULL == msg) ? "bytes" : "iovecs",
                         ORTE_NAME_PRINT(recipient),
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
    if (NULL == orcm_pnp_base.my_string_id) {
        return ORCM_ERR_NOT_AVAILABLE;
    }

    if (!orcm_pnp_base.comm_enabled) {
        return ORCM_ERR_COMM_DISABLED;
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
    if (ORTE_SUCCESS != (ret = orcm_pnp_base_construct_msg(&buf, buffer, tag, msg, count))) {
        ORTE_ERROR_LOG(ret);
        ORTE_RELEASE_THREAD(&local_thread);
        return ret;
    }
    
    /* if this is intended for everyone who might be listening to my output,
     * multicast it
     */
    if (NULL == recipient ||
        (ORTE_JOBID_WILDCARD == recipient->jobid &&
         ORTE_VPID_WILDCARD == recipient->vpid)) {
        /* if this is going on the group channel, then substitute that channel here */
        if (ORCM_PNP_GROUP_OUTPUT_CHANNEL == channel) {
            chan = orcm_pnp_base.my_output_channel->channel;
        } else if (ORCM_PNP_GROUP_INPUT_CHANNEL == channel) {
            chan = orcm_pnp_base.my_input_channel->channel;
        } else {
            chan = channel;
        }
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:sending_nb multicast of %d %s to channel %s tag %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (NULL == msg) ? (int)buffer->bytes_used : count,
                             (NULL == msg) ? "bytes" : "iovecs",
                             orcm_pnp_print_channel(channel),
                             orcm_pnp_print_tag(tag)));
        
        /* release thread prior to send */
        ORTE_RELEASE_THREAD(&local_thread);
        /* send the data to the channel */
        if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer_nb(chan, tag, buf,
                                                              rmcast_callback, send))) {
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
        OBJ_RELEASE(send);
        ORTE_RELEASE_THREAD(&local_thread);
        return ORTE_ERR_NOT_IMPLEMENTED;
    }
    
    /* intended for a specific recipient, send it over p2p */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending_nb p2p message of %d %s to %s tag %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == msg) ? (int)buffer->bytes_used : count,
                         (NULL == msg) ? "bytes" : "iovecs",
                         ORTE_NAME_PRINT(recipient),
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
    if (NULL == orcm_pnp_base.my_string_id) {
        return NULL;
    }

    return strdup(orcm_pnp_base.my_string_id);
}

static int disable_comm(void)
{
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default: disabling comm",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));

    orcm_pnp_base.comm_enabled = false;

    /* stop the rmcast framework */
    orte_rmcast.disable_comm();

    /* cancel the recvs, if active */
    if (recv_on) {
        orte_rmcast.cancel_recv(ORTE_RMCAST_WILDCARD_CHANNEL, ORTE_RMCAST_TAG_WILDCARD);
        orte_rml.recv_cancel(ORTE_NAME_WILDCARD, ORTE_RML_TAG_MULTICAST_DIRECT);
        recv_on = false;
    }

    /* stop the processing thread */
    orcm_pnp_base_stop_threads();
}


static int default_finalize(void)
{
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default: finalizing",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    if (orcm_pnp_base.comm_enabled) {
        disable_comm();
    }

    /* destruct the threading support */
    OBJ_DESTRUCT(&local_thread);
    
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
    
    OBJ_RELEASE(buf);
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
    
    OBJ_RELEASE(buffer);
    /* do any required callbacks */
    if (NULL != send->cbfunc) {
        send->cbfunc(status, ORTE_PROC_MY_NAME, send->tag, send->msg, send->count, send->buffer, send->cbdata);
    }
    OBJ_RELEASE(send);
}

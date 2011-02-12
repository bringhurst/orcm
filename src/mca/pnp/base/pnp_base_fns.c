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

static void cbfunc(int status,
                   orte_process_name_t *sender,
                   orcm_pnp_tag_t tag,
                   struct iovec *msg,
                   int count,
                   opal_buffer_t *buffer,
                   void *cbdata)
{
    OBJ_RELEASE(buffer);
}

void orcm_pnp_base_process_announcements(orte_process_name_t *sender,
                                         opal_buffer_t *buf)
{
    opal_list_item_t *itm2;
    orcm_triplet_t *triplet;
    orcm_triplet_group_t *grp;
    orcm_source_t *source;
    char *app=NULL, *version=NULL, *release=NULL, *string_id=NULL, *nodename=NULL;
    orte_process_name_t originator;
    opal_buffer_t *ann;
    int rc, n, i, j;
    orte_rmcast_channel_t input, output;
    orcm_pnp_send_t *pkt;
    orcm_pnp_channel_t chan;
    orte_job_t *daemons;
    orte_proc_t *proc;
    uint32_t uid;
    bool known=true;
    char *rml_uri=NULL;
    pid_t pid;
    orcm_info_t info;
    int32_t incarnation;

    /* unpack the sender's triplet */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &string_id, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* get its input multicast channel */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &input, &n, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* get its output multicast channel */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &output, &n, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* get its nodename */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &nodename, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* get its uid */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &uid, &n, OPAL_UINT32))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* unpack the its rml uri */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &rml_uri, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* unpack its pid */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &pid, &n, OPAL_PID))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* unpack its incarnation */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &incarnation, &n, OPAL_INT32))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* set the contact info - this has to be done even if the source is known
     * as it could be a repeat invocation of the same application
     */
    if (ORTE_SUCCESS != (rc = orte_rml.set_contact_info(rml_uri))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* if the job family is different, then this wasn't launched
     * by our dvm, so we won't already know the route to it.
     */
    if (ORTE_JOB_FAMILY(ORTE_PROC_MY_NAME->jobid) != ORTE_JOB_FAMILY(sender->jobid)) {
        /* set the route to be direct */
        if (ORTE_SUCCESS != (rc = orte_routed.update_route(sender, sender))) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    /* who are they responding to? */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &originator, &n, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:base:received announcement from app %s channel %s on node %s uid %u",
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
    orcm_pnp_base_check_pending_recvs(triplet, grp);

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
        /* if the source is dead, then it is restarting, so
         * declare it as unknown so the announce cbfunc
         * will be executed
         */
        if (!source->alive) {
            known = false;
        }
        /* flag it as alive */
        source->alive = true;
        /* the source returns locked, so release it */
        ORTE_RELEASE_THREAD(&source->ctl);
    }
    /* release the triplet thread */
    ORTE_RELEASE_THREAD(&triplet->ctl);

    /* notify the user, if requested - be sure to do
     * this outside of the triplet lock in case the cbfunc
     * needs to access the triplet
     */
    if (NULL != grp->pnp_cbfunc) {
        /* if the user requested a callback, they probably intend to send
         * something to this triplet - so ensure the channel to its input is open
         */
        if (ORTE_SUCCESS != (rc = orte_rmcast.open_channel(input, string_id, NULL, -1, NULL, ORTE_RMCAST_XMIT))) {
            ORTE_ERROR_LOG(rc);
            ORTE_RELEASE_THREAD(&triplet->ctl);
            goto cleanup;
        }
        grp->pnp_cbfunc(app, version, release, input);
        /* flag that the callback for this jobid/grp has been done */
        grp->pnp_cb_done = true;
        grp->pnp_cbfunc = NULL;
    }

    /* if this is a new source and they wanted a callback,
     * now is the time to do it in case it needs to do some
     * prep before we can send our return announcement
     */
    if (!known && NULL != orcm_pnp_base.my_announce_cbfunc) {
        info.app = app;
        info.version = version;
        info.release = release;
        info.name = sender;
        info.nodename = nodename;
        info.rml_uri = rml_uri;
        info.uid = uid;
        info.pid = pid;
        info.incarnation = incarnation;
        orcm_pnp_base.my_announce_cbfunc(&info);
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:base: announcement sent in response to originator %s",
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
                             "%s pnp:base:recvd_ann response to another announce - ignoring",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        goto cleanup;
    }
    
    /* if we get here, then this is an original announcement */
    if (ORCM_PROC_IS_APP) {
        chan = ORTE_RMCAST_APP_PUBLIC_CHANNEL;
    } else {
        chan = ORTE_RMCAST_SYS_CHANNEL;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:base:received announcement sending response",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* assemble the announcement response */
    ann = OBJ_NEW(opal_buffer_t);
    
    /* pack the common elements */
    if (ORCM_SUCCESS != (rc = orcm_pnp_base_pack_announcement(ann, sender))) {
        if (ORCM_ERR_NOT_AVAILABLE != rc) {
            /* not-avail => have not announced ourselves yet */
            ORTE_ERROR_LOG(rc);
        }
        OBJ_RELEASE(ann);
        goto cleanup;
    }
    
    /* send it */
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(chan, NULL,
                                                 ORCM_PNP_TAG_ANNOUNCE,
                                                 NULL, 0, ann, cbfunc, NULL))) {
        /* protect against a race condition */
        if (ORTE_ERR_COMM_DISABLED != rc) {
            ORTE_ERROR_LOG(rc);
        }
    }
    
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
    if (NULL != nodename) {
        free(nodename);
    }
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:base:recvd_announce complete",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    return;
}

/* pack the common elements of an announcement message */
int orcm_pnp_base_pack_announcement(opal_buffer_t *buf, orte_process_name_t *sender)
{
    int ret;
    char *rml_uri;
    orcm_pnp_channel_t chan;

    /* if we haven't registered an app-triplet yet, then we can't announce */
    if (NULL == orcm_pnp_base.my_string_id || !orcm_pnp_base.comm_enabled) {
        return ORCM_ERR_NOT_AVAILABLE;
    }
    
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &orcm_pnp_base.my_string_id, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    /* pack my input channel */
    if (NULL != orcm_pnp_base.my_input_channel) {
        chan = orcm_pnp_base.my_input_channel->channel;
    } else {
        chan = ORCM_PNP_INVALID_CHANNEL;
    }
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &chan, 1, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }

    /* pack my output channel */
    if (NULL != orcm_pnp_base.my_output_channel) {
        chan = orcm_pnp_base.my_output_channel->channel;
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
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &orcm_pnp_base.my_uid, 1, OPAL_UINT32))) {
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
    
    /* pack my pid - useful for debugging */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &orte_process_info.pid, 1, OPAL_PID))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }

    /* pack my incarnation number */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &orte_process_info.num_restarts, 1, OPAL_INT32))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }

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

orcm_pnp_request_t* orcm_pnp_base_find_request(opal_list_t *list,
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

int orcm_pnp_base_construct_msg(opal_buffer_t **buf, opal_buffer_t *buffer,
                                orcm_pnp_tag_t tag, struct iovec *msg, int count)
{
    int ret;
    int8_t flag;
    int sz;
    int32_t cnt;

    *buf = OBJ_NEW(opal_buffer_t);
    
    /* insert our string_id */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(*buf, &orcm_pnp_base.my_string_id, 1, OPAL_STRING))) {
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
void orcm_pnp_base_check_trip_recvs(char *stringid,
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
                if (NULL == (recvr = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&orcm_pnp_base.channels, channel))) {
                    recvr = OBJ_NEW(orcm_pnp_channel_obj_t);
                    recvr->channel = channel;
                    opal_pointer_array_set_item(&orcm_pnp_base.channels, recvr->channel, recvr);
                }
            }
            /* see if this recv is already present */
            if (NULL == (reqcp = orcm_pnp_base_find_request(&recvr->recvs, req->string_id, req->tag))) {
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
            if (NULL != orcm_pnp_base.my_string_id &&
                recvr == orcm_pnp_base.my_input_channel &&
                0 == strcasecmp(orcm_pnp_base.my_string_id, req->string_id)) {
                free(reqcp->string_id);
                reqcp->string_id = strdup(ORCM_WILDCARD_STRING_ID);
            }
            /* update the cbfunc */
            reqcp->cbfunc = req->cbfunc;
            reqcp->cbdata = req->cbdata;

            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s pnp:base:check_trip_recvs putting recv for %s:%s on channel %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), reqcp->string_id,
                                 orcm_pnp_print_tag(reqcp->tag),
                                 orcm_pnp_print_channel(recvr->channel)));
        }
        /* if we have any recvs now, ensure the channel is open */
        if (NULL != recvr && 0 < opal_list_get_size(&recvr->recvs)) {
            /* open the channel */
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s pnp:base:check_pending_recvs setup recv for channel %s",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), orcm_pnp_print_channel(recvr->channel)));
            if (ORTE_SUCCESS != (rc = orte_rmcast.open_channel(recvr->channel, stringid,
                                                               NULL, -1, NULL, ORTE_RMCAST_RECV))) {
                ORTE_ERROR_LOG(rc);
            }
            /* setup the recv */
            if (ORTE_SUCCESS != (rc = orte_rmcast.recv_buffer_nb(recvr->channel,
                                                                 ORTE_RMCAST_TAG_WILDCARD,
                                                                 ORTE_RMCAST_PERSISTENT,
                                                                 orcm_pnp_base_recv_input_buffers, NULL))) {
                ORTE_ERROR_LOG(rc);
            }
        }
    }
}

void orcm_pnp_base_check_pending_recvs(orcm_triplet_t *trp,
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
                orcm_pnp_base_check_trip_recvs(triplet->string_id, &triplet->input_recvs, grp->input);
            }
            if (ORCM_PNP_INVALID_CHANNEL != grp->output) {
                orcm_pnp_base_check_trip_recvs(triplet->string_id, &triplet->output_recvs, grp->output);
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
        orcm_pnp_base_check_trip_recvs(trp->string_id, &trp->input_recvs, grp->input);
    }

    /* check the triplet output_recv list */
    if (ORCM_PNP_INVALID_CHANNEL != grp->output) {
        orcm_pnp_base_check_trip_recvs(trp->string_id, &trp->output_recvs, grp->output);
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
void orcm_pnp_base_update_pending_recvs(orcm_triplet_t *trp)
{
    orcm_triplet_group_t *grp;
    int i;

    for (i=0; i < trp->groups.size; i++) {
        if (NULL == (grp = (orcm_triplet_group_t*)opal_pointer_array_get_item(&trp->groups, i))) {
            continue;
        }
        orcm_pnp_base_check_trip_recvs(trp->string_id, &trp->input_recvs, grp->input);
        if (grp->input != grp->output) {
            orcm_pnp_base_check_trip_recvs(trp->string_id, &trp->output_recvs, grp->output);
        }
    }
}

int orcm_pnp_base_record_recv(orcm_triplet_t *triplet,
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
        if (triplet == orcm_pnp_base.my_triplet) {
            if (NULL != orcm_pnp_base_find_request(&triplet->input_recvs, ORCM_WILDCARD_STRING_ID, tag)) {
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
            if (NULL != orcm_pnp_base_find_request(&triplet->input_recvs, triplet->string_id, tag)) {
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
        orcm_pnp_base_update_pending_recvs(triplet);
        goto cleanup;
    } else if (ORCM_PNP_GROUP_OUTPUT_CHANNEL == channel) {
        /* see if the request already exists on the triplet's output queue */
        if (NULL != orcm_pnp_base_find_request(&triplet->output_recvs, triplet->string_id, tag)) {
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
        orcm_pnp_base_update_pending_recvs(triplet);
        goto cleanup;
    } else {
        /* get the specified channel */
        if (NULL == (chan = (orcm_pnp_channel_obj_t*)opal_pointer_array_get_item(&orcm_pnp_base.channels, channel))) {
            chan = OBJ_NEW(orcm_pnp_channel_obj_t);
            chan->channel = channel;
            opal_pointer_array_set_item(&orcm_pnp_base.channels, chan->channel, chan);
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
                                                          orcm_pnp_base_recv_input_buffers, NULL))) {
        if (ORTE_EXISTS == ret) {
            ret = ORTE_SUCCESS;
            goto cleanup;
        }
        ORTE_ERROR_LOG(ret);
    }

 cleanup:
    return ret;
}

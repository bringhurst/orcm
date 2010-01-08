/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config_private.h"
#include "constants.h"

#include <confd.h>
#include <confd_cdb.h>

#include "opal/dss/dss.h"
#include "opal/util/output.h"
#include "opal/util/argv.h"
#include "opal/util/path.h"

#include "orte/mca/rmcast/rmcast.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"

#include "mca/clip/clip.h"

#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/base/private.h"
#include "mca/cfgi/base/public.h"
#include "mca/cfgi/confd/cfgi_confd.h"

/* API functions */

static int confd_init(void);
static void confd_read_config(orcm_spawn_fn_t spawn_app);
static int confd_finalize(void);

/* The module struct */

orcm_cfgi_base_module_t orcm_cfgi_confd_module = {
    confd_init,
    confd_read_config,
    confd_finalize
};

/* local functions */
static void recv_subdata(int sd, short flags, void *subscription);

/* local variables */
static int subsocket, datasocket;
static opal_event_t configev;

static int confd_init(void)
{
    struct sockaddr_in addr;

    /* setup the address to the db */
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONFD_PORT);
    
    /* identify mode for connecting to the db */
    if (0 == strcasecmp(mca_cfgi_confd_component.mode, "SILENT")) {
        mode = CONFD_SILENT;
    } else if (0 == strcasecmp(mca_cfgi_confd_component.mode, "DEBUG")) {
        mode = CONFD_DEBUG;
    } else {
        mode = CONFD_TRACE;
    }
    
    /* init the database */
    if (CONFD_OK != confd_init(argv[0], mca_cfgi_confd_component.log_stream, mode)) {
        opal_output(0, "Could not connect to confd database");
        return ORTE_ERR_NOT_FOUND;
    }
    
    /*
     * so confd_pp_kpath() can print text path component names
     * (though enum values aren't printed by confd_ns_pp_value)
     */
    confd_load_schemas((struct sockaddr*) &addr, sizeof(addr));
    
    return ORCM_SUCCESS;
}

static enum cdb_iter_ret process_subs(confd_hkeypath_t *kp,
                                      enum cdb_iter_op  op,
                                      confd_value_t    *oldv, /* requires ITER_WANT_PREV */
                                      confd_value_t    *newv,
                                      void             *state)
{
    orcm_cfgi_sub_pair_t *pair = (orcm_cfgi_sub_pair_t*)state;
    
    /* create a new change object */
    leaf = OBJ_NEW(orcm_cfgi_sub_result_t);

    /* save the original path */
    leaf->path = strdup(pair->sub->paths[pair->id]);
    
    /* extract the changed path */
    confd_pp_kpath(leaf->changed, BUFSIZ, kp);
    
    switch (op) {
        case MOP_CREATED:   leaf->op = ORCM_OP_CREATED;  break;
        case MOP_DELETED:   leaf->op = ORCM_OP_DELETED;  break;
        case MOP_VALUE_SET: leaf->op = ORCM_OP_SET;      break;
        case MOP_MODIFIED:  leaf->op = ORCM_OP_MODIFIED; break;
    }
    
    /* get the values */
    if (oldv) {
        confd_pp_value(leaf->oldvalue, BUFSIZ, oldv);
    }
    if (newv) {
        confd_pp_value(leaf->newvalue, BUFSIZ, newv);
    }
    
    opal_dss.dump(0, leaf, ORCM_SUB_RESULT);
    
    /* add data to list of returned values */
    opal_list_append(&sub->returned_data, &leaf->super);
    
    return ITER_RECURSE;
}
                    
static void recv_subdata(int sd, short flags, void *subscription)
{
    orcm_cfgi_subscription_t *sub = (orcm_cfgi_subscription_t*)subscription;
    int nsubev, *subev=NULL;
    enum cdb_sub_notification sub_notify_type;
    int i, j;
    int rc = ORCM_SUCCESS;
    orcm_cfgi_sub_pair_t pair;
    
    /* read the data from confd */
    if (CONFD_OK != cdb_read_subscription_socket2(sub->socket,
                                                        &sub_notify_type,
                                                        NULL,     /* no interest in flags */
                                                        &subev,
                                                        &nsubev))) {
        rc = ORCM_ERR_FATAL;
        goto cleanup;
    }
    
    /* store the notify type, converting it to something abstract */
    if (CDB_SUB_PREPARE == sub_notify_type) {
        sub->notify_type = ORCM_CFGI_PREPARE;
    } else if (CDB_SUB_COMMIT == sub_notify_type) {
        sub->notify_type = ORCM_CFGI_COMMIT;
    } else if (CDB_SUB_ABORT == sub_notify_type) {
        sub->notify_type = ORCM_CFGI_ABORT;
    } else if (CDB_SUB_OPER == sub_notify_type) {
        sub->notify_type = ORCM_CFGI_OPER;
    } else {
        sub-notify_type = ORCM_CFGI_UNKNOWN;
    }
    
    /* process the subscription events */
    pair.sub = sub;
    for (i=0; i < nsubev; i++) {
        /* find this subscription pt */
        for (j=0; j < sub->num_subpts; j++) {
            if (subev[j] == OPAL_VALUE_ARRAY_GET_ITEM(&sub->subpts, int, j)) {
                /* found it */
                pair.id = j;
                if (CONFD_OK != cdb_diff_iterate(sub->socket, subev[j],
                                                 process_subs,
                                                 ITER_WANT_PREV|ITER_WANT_ANCESTOR_DELETE,
                                                 &pair)) {
                    opal_output(0, "cdb_diff_iterate failed on subpt %d with error %s",
                                subev[j], confd_last_err());
                    rc = ORCM_ERR_FATAL;
                    goto cleanup;
                }
                break;
            }
        }
    }
    
    /* now execute the user's callback for this subscription */
    sub->cbfunc(sub->notify_type, &sub->returned_data);
    
cleanup:
    if (NULL != subev) {
        free(subev);
    }
    
    /* cleanup the list of returned data */
    while (NULL != (item = opal_list_remove_first(&sub->returned_data))) {
        OBJ_RELEASE(item);
    }
    
    /* sync the database */
    if (CONFD_OK != cdb_sync_subscription_socket(sub->socket, CDB_DONE_PRIORITY)) {
        opal_output(0, "Failed to sync confd: %s", confd_last_err());
        rc = ORCM_ERR_FATAL;
    }

    return rc;
}

/* get our configuration */
static int confd_subscribe(char **configpts, int priority, orcm_cfgi_cbfunc_t cbfunc)
{
    orcm_cfgi_subscription_t *sub;
    int subpt;
    
    /* get a new subscription */
    sub = OBJ_NEW(orcm_cfgi_subscription_t);
    sub->cbfunc = cbfunc;
    
    /* set the value array size to match the number of
     * sub pts we are being given
     */
    opal_value_array_set_size(&sub->subpts, opal_argv_count(configpts));
    sub->num_subpts = opal_argv_count(configpts);
    
    /* setup a subscription socket */
    if (0 > (sub->socket = socket(PF_INET, SOCK_STREAM, 0))) {
        opal_output(0, "Could not get subscription socket");
        OBJ_RELEASE(sub);
        return ORCM_ERR_FATAL;
    }
    
    /* connect to the database subscription socket */
    if (0 > cbd_connect(sub->socket, CDB_SUBSCRIPTION_SOCKET, (struct sockaddr*)&addr,
                        sizeof (struct sockaddr_in))) {
        opal_output(0, "Could not connect to confd subscription socket");
        OBJ_RELEASE(sub);
        return ORCM_ERR_FATAL;
    }
    
    /* subscribe to all the points given to us */
    for (i=0; NULL != configpts[i]; i++) {
        if (CONFD_OK != cdb_subscribe(sub->socket, priority, mca_cfgi_confd_component.ns,
                                      &subpt, configpt)) {
            opal_output(0, "Subscription to config %s failed", configpt);
            OBJ_RELEASE(sub);
            return ORCM_ERR_REQUEST_FAILED;
        }
        OPAL_VALUE_ARRAY_SET_ITEM(&sub->subpts, int, i, subpt);
        opal_argv_array_add_nosize(&sub->paths, configpt);
    }
    
    if (CONFD_OK != cbd_subscribe_done(sub->socket)) {
        opal_output(0, "Subscribe done failed");
        OBJ_RELEASE(sub);
        return ORCM_ERR_REQUEST_FAILED;
    }

    /* add subscription to our array */
    sub->subpt = opal_pointer_array_add(&subs, sub);
    
    /* being monitoring the socket */
    opal_event_set(&sub->ev, sub->socket, OPAL_EV_READ, recv_subdata, sub); 
    opal_event_add(&sub->ev, 0);
    
    return sub->subpt;
}

static int confd_cancel_subscription(int sub_id)
{
    orcm_cfgi_subscription_t *sub, *found;

    /* get this subscription */
    if (NULL == (sub = (orcm_cfgi_subscription_t*)opal_pointer_array_get_item(&subs, sub_id))) {
        return ORCM_ERR_NOT_FOUND;
    }
    
    /* cancel the subscription by closing the socket */
    cdb_close(sub->socket);
    /* release the info */
    OBJ_RELEASE(sub);
    
    /* NULL the array location */
    opal_pointer_array_set_item(&subs, sub_id, NULL);
    
    return ORCM_SUCCESS;
}

static int confd_finalize(void)
{
    /* cancel all subscriptions */
    for (i=0; i < subs.size; i++) {
        if (NULL == (sub = (orcm_cfgi_subscription_t*)opal_pointer_array_get_item(&subs, sub_id))) {
            continue;
        }
        cdb_close(sub->socket);
        OBJ_RELEASE(sub);
    }
    OBJ_DESTRUCT(&subs);
    
    /* close the db */
    
    return ORCM_SUCCESS;
}


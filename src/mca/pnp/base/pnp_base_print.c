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

#include "opal/util/printf.h"
#include "opal/util/opal_sos.h"
#include "opal/threads/tsd.h"

#include "orte/mca/errmgr/errmgr.h"

#include "mca/pnp/pnp_types.h"
#include "mca/pnp/base/private.h"

#define ORCM_PRINT_MAX_SIZE   50
#define ORCM_PRINT_NUM_BUFS   16

static bool fns_init=false;

static opal_tsd_key_t print_tsd_key;
static char* orcm_print_null = "NULL";
typedef struct {
    char *buffers[ORCM_PRINT_NUM_BUFS];
    int cntr;
} orcm_print_buffers_t;

static void buffer_cleanup(void *value)
{
    int i;
    orcm_print_buffers_t *ptr;
    
    if (NULL != value) {
        ptr = (orcm_print_buffers_t*)value;
        for (i=0; i < ORCM_PRINT_NUM_BUFS; i++) {
            free(ptr->buffers[i]);
        }
    }
}

static orcm_print_buffers_t *get_print_buffer(void)
{
    orcm_print_buffers_t *ptr;
    int ret, i;
    
    if (!fns_init) {
        /* setup the print_args function */
        if (ORCM_SUCCESS != (ret = opal_tsd_key_create(&print_tsd_key, buffer_cleanup))) {
            ORTE_ERROR_LOG(ret);
            return NULL;
        }
        fns_init = true;
    }
    
    ret = opal_tsd_getspecific(print_tsd_key, (void**)&ptr);
    if (OPAL_SUCCESS != ret) return NULL;
    
    if (NULL == ptr) {
        ptr = (orcm_print_buffers_t*)malloc(sizeof(orcm_print_buffers_t));
        for (i=0; i < ORCM_PRINT_NUM_BUFS; i++) {
            ptr->buffers[i] = (char *) malloc((ORCM_PRINT_MAX_SIZE+1) * sizeof(char));
        }
        ptr->cntr = 0;
        ret = opal_tsd_setspecific(print_tsd_key, (void*)ptr);
    }
    
    return (orcm_print_buffers_t*) ptr;
}

/* Simple function to pretty-print tags */
char* orcm_pnp_print_tag(orcm_pnp_tag_t tag)
{
    char *ret;
    orcm_print_buffers_t *ptr;

    switch(tag) {
    case ORCM_PNP_TAG_WILDCARD:
        ret = "WILDCARD";
        break;
    case ORCM_PNP_TAG_INVALID:
        ret = "INVALID";
        break;
    case ORCM_PNP_TAG_BOOTSTRAP:
        ret = "BOOTSTRAP";
        break;
    case ORCM_PNP_TAG_ANNOUNCE:
        ret = "ANNOUNCE";
        break;
    case ORCM_PNP_TAG_OUTPUT:
        ret = "OUTPUT";
        break;
    case ORCM_PNP_TAG_PS:
        ret = "PS";
        break;
    case ORCM_PNP_TAG_MSG:
        ret = "MSG";
        break;
    case ORCM_PNP_TAG_TOOL:
        ret = "TOOL";
        break;
    case ORCM_PNP_TAG_IOF:
        ret = "IOF";
        break;
    case ORCM_PNP_TAG_DATA:
        ret = "DATA";
        break;
    case ORCM_PNP_TAG_CMD_ACK:
        ret = "CMD_ACK";
        break;
    case ORCM_PNP_TAG_HEARTBEAT:
        ret = "HEARTBEAT";
        break;
    case ORCM_PNP_TAG_COMMAND:
        ret = "COMMAND";
        break;
    case ORCM_PNP_TAG_ERRMGR:
        ret = "ERRMGR";
        break;
    default:
        /* not a system-defined tag - so print the value out */
        ptr = get_print_buffer();
        if (NULL == ptr) {
            ORTE_ERROR_LOG(ORCM_ERR_OUT_OF_RESOURCE);
            ret = orcm_print_null;
            break;
        }
        /* cycle around the ring */
        if (ORCM_PRINT_NUM_BUFS == ptr->cntr) {
            ptr->cntr = 0;
        }
        snprintf(ptr->buffers[ptr->cntr], ORCM_PRINT_MAX_SIZE, "%d", tag);
        ret = ptr->buffers[ptr->cntr];
        ptr->cntr++;
        break;
    }
    return ret;
}

char *orcm_pnp_print_channel(orcm_pnp_channel_t chan)
{
    orcm_print_buffers_t *ptr;
    char *ret;

    switch(chan) {
    case ORCM_PNP_GROUP_INPUT_CHANNEL:
        ret = "INPUT";
        break;
    case ORCM_PNP_GROUP_OUTPUT_CHANNEL:
        ret = "OUTPUT";
        break;
    case ORCM_PNP_WILDCARD_CHANNEL:
        ret = "WILDCARD";
        break;
    case ORCM_PNP_INVALID_CHANNEL:
        ret = "INVALID";
        break;
    case ORCM_PNP_SYS_CHANNEL:
        ret = "SYSTEM";
        break;
    case ORCM_PNP_APP_PUBLIC_CHANNEL:
        ret = "PUBLIC";
        break;
    case ORCM_PNP_DATA_SERVER_CHANNEL:
        ret = "DATA";
        break;
    default:
        /* not a system-defined channel - so print the value out */
        ptr = get_print_buffer();
        if (NULL == ptr) {
            ORTE_ERROR_LOG(ORCM_ERR_OUT_OF_RESOURCE);
            ret = orcm_print_null;
            break;
        }
        /* cycle around the ring */
        if (ORCM_PRINT_NUM_BUFS == ptr->cntr) {
            ptr->cntr = 0;
        }
        snprintf(ptr->buffers[ptr->cntr], ORCM_PRINT_MAX_SIZE, "%d", chan);
        ret = ptr->buffers[ptr->cntr];
        ptr->cntr++;
        break;
    }
    return ret;
}

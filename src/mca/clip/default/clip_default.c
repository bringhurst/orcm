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

#include "opal/dss/dss.h"
#include "opal/util/output.h"

#include "orte/mca/rmcast/rmcast.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"

#include "mca/clip/clip.h"
#include "mca/clip/base/private.h"
#include "mca/clip/default/clip_default.h"

/* API functions */

static int default_init(void);
static int default_replicate(void);
static int default_finalize(void);

/* The module struct */

orcm_clip_base_module_t orcm_clip_default_module = {
    default_init,
    default_replicate,
    default_finalize
};

/* local function */
static char *cm_getline(FILE *fp);

static int default_init(void)
{
    return ORCM_SUCCESS;
}

static int default_finalize(void)
{
    return ORCM_SUCCESS;
}

static int default_replicate(void)
{
#if 0
    char *file, *cmd;
    int i, nreps=0;
    FILE *fp;
    
    /* backdoor access the fault group file */
    mca_base_param_reg_string("rmaps_resilient", "fault_grp_file",
                               "Filename that contains a description of fault groups for this system",
                               false, false, NULL,  &file);
    if (NULL == file || (NULL == (fp = fopen(file, "r")))) {
        /* just default to num nodes in system */
        for (i=0; i < orte_node_pool->size; i++) {
            if (NULL == opal_pointer_array_get_item(orte_node_pool, i)) {
                continue;
            }
            nreps++;
        }
        return nreps;
    }
    
    /* count the number of fault groups */
    while (NULL != (cmd = cm_getline(fp))) {
        nreps++;
        free(cmd);
    }
    fclose(fp);
    
    return nreps;
#endif
    return -1;
}

#define CM_MAX_LINE_LENGTH  1024

static char *cm_getline(FILE *fp)
{
    char *ret, *buff;
    char input[CM_MAX_LINE_LENGTH];
    
retry:
    ret = fgets(input, CM_MAX_LINE_LENGTH, fp);
    if (NULL != ret) {
        if ('#' == input[0]) {
            /* ignore this line - it is a comment */
            goto retry;
        }
        input[strlen(input)-1] = '\0';  /* remove newline */
        buff = strdup(input);
        return buff;
    }
    
    return NULL;
}


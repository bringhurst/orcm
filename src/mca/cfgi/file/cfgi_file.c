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
#include "mca/cfgi/file/cfgi_file.h"

/* API functions */

static int file_init(void);
static void file_read_config(orcm_spawn_fn_t spawn_app);
static int file_finalize(void);

/* The module struct */

orcm_cfgi_base_module_t orcm_cfgi_file_module = {
    file_init,
    file_read_config,
    file_finalize
};

/* local functions */
static char *cm_getline(FILE *fp);

static int file_init(void)
{
    return ORCM_SUCCESS;
}

/* file will contain a set of key-value pairs:
 * app-grp:
 *     key=value
 *
 * ends either at end-of-file or next app-grp. empty
 * lines are ignored
 */
static void file_read_config(orcm_spawn_fn_t spawn_app)
{
    int ret, i;
    FILE *fp;
    char *cmd;
    char **inpt, **xfer;
    char *app;
    int num_apps;
    
    fp = fopen(mca_cfgi_file_component.file, "r");
    if (NULL == fp) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        return;
    }
    while (NULL != (cmd = cm_getline(fp))) {
        OPAL_OUTPUT_VERBOSE((2, orcm_cfgi_base.output,
                             "%s file_init: launching cmd %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), cmd));
        inpt = opal_argv_split(cmd, ' ');
        free(cmd);
        /* get the absolute path */
        if (NULL == (app = opal_find_absolute_path(inpt[0]))) {
            fprintf(stderr, "App %s could not be found - try changing path\n", inpt[0]);
            continue;
        }
        xfer = NULL;
        opal_argv_append_nosize(&xfer, app);
        /* get the number of instances */
        num_apps = strtol(inpt[1], NULL, 10);
        /* if they want us to auto-set it, do so */
        if (num_apps < 0) {
            num_apps = orcm_clip.replicate();
        }
        /* add any args */
        for (i=2; NULL != inpt[i]; i++) {
            opal_argv_append_nosize(&xfer, inpt[i]);
        }
        opal_argv_free(inpt);
        cmd = opal_argv_join(xfer, ' ');
        opal_argv_free(xfer);
        ORCM_SPAWN_EVENT(cmd, false, false, INT32_MAX, num_apps, NULL, false, spawn_app);
        free(cmd);
    }
    fclose(fp);
    
    return;
}

static int file_finalize(void)
{
    return ORCM_SUCCESS;
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


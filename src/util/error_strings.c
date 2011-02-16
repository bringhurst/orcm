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

#include <stdio.h>

int orcm_err2str(int errnum, const char **errmsg)
{
    switch (errnum) {
        case ORCM_ERR_PLACEHOLDER:
            *errmsg = "Placeholder";
            break;
        default:
            *errmsg = NULL;
    }

    return ORCM_SUCCESS;
}

/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file **/

#include "openrcm_config.h"
#include "constants.h"

#include <stdio.h>

const char *
orcm_err2str(int errnum)
{
    const char *retval;
    switch (errnum) {
        case ORCM_ERR_PLACEHOLDER:
            retval = "Placeholder";
            break;
        default:
            retval = NULL;
    }

    return retval;
}

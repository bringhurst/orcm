/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2010 Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config_private.h"
#include "include/constants.h"

#include "orte_config.h"
#include "orte/constants.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#ifdef HAVE_STRING_H
#include <string.h>
#endif  /* HAVE_STRING_H */
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#else
#ifdef HAVE_SYS_FCNTL_H
#include <sys/fcntl.h>
#endif
#endif

#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"

#include "orte/mca/iof/iof.h"
#include "orte/mca/iof/base/base.h"

#include "mca/pnp/pnp.h"

#include "iof_orcm.h"

static orte_job_t *daemons=NULL;
static uint16_t dfam;

void orte_iof_orcm_recv(int status,
                        orte_process_name_t *sender,
                        orcm_pnp_tag_t tag,
                        struct iovec *mg,
                        int cnt,
                        opal_buffer_t *buffer,
                        void *cbdata)
{
    orte_process_name_t origin;
    unsigned char data[ORTE_IOF_BASE_MSG_MAX];
    orte_iof_tag_t stream;
    int32_t count, numbytes;
    orte_iof_sink_t *sink;
    opal_list_item_t *item, *next;
    int rc;
    uint16_t jfam;
    
    if (NULL == daemons) {
        if (NULL == (daemons = orte_get_job_data_object(0))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            return;
        }
        dfam = ORTE_JOB_FAMILY(daemons->jobid);
    }

    /* if this isn't from the DVM I am scheduling, ignore it */
    count=1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &jfam, &count, OPAL_UINT16))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    if (jfam != dfam) {
        opal_output(0, "%s IOF FROM DVM %s - NOT FOR ME!",
                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                    ORTE_JOB_FAMILY_PRINT(jfam));
        return;
    }

    /* unpack the stream */
    count = 1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &stream, &count, ORTE_IOF_TAG))) {
        ORTE_ERROR_LOG(rc);
        goto CLEAN_RETURN;
    }
    
    /* get name of the process whose output we are handling */
    count = 1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &origin, &count, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        goto CLEAN_RETURN;
    }
    
    /* unpack the data */
    numbytes=ORTE_IOF_BASE_MSG_MAX;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, data, &numbytes, OPAL_BYTE))) {
        ORTE_ERROR_LOG(rc);
        goto CLEAN_RETURN;
    }
    /* numbytes will contain the actual #bytes that were sent */
    
    OPAL_OUTPUT_VERBOSE((1, orte_iof_base.iof_output,
                         "%s unpacked %d bytes from remote proc %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), numbytes,
                         ORTE_NAME_PRINT(&origin)));
    
    /* output this to our local output */
    if (ORTE_IOF_STDOUT & stream || orte_xml_output) {
        orte_iof_base_write_output(&origin, stream, data, numbytes, orte_iof_base.iof_write_stdout->wev);
    } else {
        orte_iof_base_write_output(&origin, stream, data, numbytes, orte_iof_base.iof_write_stderr->wev);
    }
    
CLEAN_RETURN:
    return;
}

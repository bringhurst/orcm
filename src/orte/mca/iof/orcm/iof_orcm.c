/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
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
#include "opal/util/output.h"
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


#include "orte/runtime/orte_globals.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"
#include "orte/mca/odls/odls_types.h"
#include "orte/mca/iof/base/base.h"

#include "mca/pnp/pnp.h"

#include "iof_orcm.h"

/* API FUNCTIONS */
static int init(void);

static int orcm_push(const orte_process_name_t* dst_name, orte_iof_tag_t src_tag, int fd);

static int orcm_pull(const orte_process_name_t* src_name,
                orte_iof_tag_t src_tag,
                int fd);

static int orcm_close(const orte_process_name_t* peer,
                 orte_iof_tag_t source_tag);

static int finalize(void);

static int orcm_ft_event(int state);

orte_iof_base_module_t orte_iof_orcm_module = {
    init,
    orcm_push,
    orcm_pull,
    orcm_close,
    finalize,
    orcm_ft_event
};


static int init(void)
{
    return ORTE_SUCCESS;
}

/* ORCM iof masters do not have local processes */
static int orcm_push(const orte_process_name_t* dst_name, orte_iof_tag_t src_tag, int fd)
{
    int ret;
    
    if (!mca_iof_orcm_component.recv_issued) {
        if (ORCM_SUCCESS != (ret = orcm_pnp.register_input_buffer("orcmd", "0.1", "alpha",
                                                                  ORCM_PNP_TAG_IOF,
                                                                  orte_iof_orcm_recv))) {
            ORTE_ERROR_LOG(ret);
        }        
    }
    return ORTE_SUCCESS;
}


/* ORCM iof masters do not have local processes */
static int orcm_pull(const orte_process_name_t* dst_name,
                     orte_iof_tag_t src_tag, int fd)
{
    return ORTE_SUCCESS;
}

/* ORCM iof masters do not have local processes */
static int orcm_close(const orte_process_name_t* peer,
                     orte_iof_tag_t source_tag)
{
    int ret;
    
    if (mca_iof_orcm_component.recv_issued) {
        if (ORCM_SUCCESS != (ret = orcm_pnp.deregister_input("orcmd", "0.1", "alpha",
                                                             ORCM_PNP_TAG_IOF))) {
            ORTE_ERROR_LOG(ret);
        }        
    }
    return ORTE_SUCCESS;
}

static int finalize(void)
{
    return ORTE_SUCCESS;
}

int orcm_ft_event(int state) {
    /*
     * Replica doesn't need to do anything for a checkpoint
     */
    return ORTE_SUCCESS;
}

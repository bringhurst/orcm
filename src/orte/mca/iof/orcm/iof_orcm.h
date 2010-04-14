/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
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
/**
 * @file
 *
 */

#ifndef ORTE_IOF_ORCM_H
#define ORTE_IOF_ORCM_H

#include "openrcm_config_private.h"
#include "include/constants.h"

#include "orte_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif  /* HAVE_SYS_TYPES_H */
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif  /* HAVE_SYS_UIO_H */
#ifdef HAVE_NET_UIO_H
#include <net/uio.h>
#endif  /* HAVE_NET_UIO_H */

#include "mca/pnp/pnp_types.h"

#include "orte/mca/iof/iof.h"
#include "orte/mca/iof/base/base.h"


BEGIN_C_DECLS

/**
 * IOF HNP Component 
 */
struct orte_iof_orcm_component_t { 
    orte_iof_base_component_t super;
    bool recv_issued;
};
typedef struct orte_iof_orcm_component_t orte_iof_orcm_component_t;

ORTE_MODULE_DECLSPEC extern orte_iof_orcm_component_t mca_iof_orcm_component;
extern orte_iof_base_module_t orte_iof_orcm_module;

void orte_iof_orcm_recv(int status,
                        orte_process_name_t *sender,
                        orcm_pnp_tag_t tag,
                        opal_buffer_t *buffer,
                        void *cbdata);

END_C_DECLS
    
#endif

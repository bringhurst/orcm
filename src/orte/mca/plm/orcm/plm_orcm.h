/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2009 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file:
 * Part of the rsh launcher. See plm_rsh.h for an overview of how it works.
 */

#ifndef ORTE_PLM_ORCM_H
#define ORTE_PLM_ORCM_H

#include "orte_config.h"

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include "opal/threads/condition.h"
#include "opal/mca/mca.h"

#include "orte/mca/plm/plm.h"

BEGIN_C_DECLS

/*
 * Module open / close
 */
int orte_plm_orcm_component_open(void);
int orte_plm_orcm_component_close(void);
int orte_plm_orcm_component_query(mca_base_module_t **module, int *priority);

/**
 * PLM Component
 */
struct orte_plm_orcm_component_t {
    orte_plm_base_component_t super;
    bool force_rsh;
    int delay;
    opal_list_t children;
    orte_std_cntr_t num_children;
    orte_std_cntr_t num_concurrent;
    opal_mutex_t lock;
    opal_condition_t cond;
};
typedef struct orte_plm_orcm_component_t orte_plm_orcm_component_t;

ORTE_MODULE_DECLSPEC extern orte_plm_orcm_component_t mca_plm_orcm_component;
ORTE_DECLSPEC extern orte_plm_base_module_t orte_plm_orcm_module;

END_C_DECLS

#endif /* ORTE_PLM_ORCM_H */
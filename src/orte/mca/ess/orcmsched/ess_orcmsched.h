/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
 *
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef ORTE_ESS_ORCMSCHED_H
#define ORTE_ESS_ORCMSCHED_H

BEGIN_C_DECLS

/*
 * Module open / close
 */
int orte_ess_orcmsched_component_open(void);
int orte_ess_orcmsched_component_close(void);
int orte_ess_orcmsched_component_query(mca_base_module_t **module, int *priority);

ORTE_MODULE_DECLSPEC extern orte_ess_base_component_t mca_ess_orcmsched_component;

END_C_DECLS

#endif /* ORTE_ESS_ORCMSCHED_H */

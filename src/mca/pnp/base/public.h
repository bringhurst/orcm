#ifndef PNP_BASE_PUBLIC_H
#define PNP_BASE_PUBLIC_H

#include "mca/pnp/pnp.h"

int orcm_pnp_base_open(void);
int orcm_pnp_base_select(void);
int orcm_pnp_base_close(void);

extern const mca_base_component_t *orcm_pnp_base_components[];

#endif

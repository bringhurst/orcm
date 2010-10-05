/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef CFGI_CONFD_H
#define CFGI_CONFD_H

#include "openrcm.h"

/* Functions in the cfgi confd component */

int orcm_cfgi_confd_component_open(void);
int orcm_cfgi_confd_component_close(void);
int orcm_cfgi_confd_component_query(mca_base_module_2_0_0_t **module, int *priority);
int orcm_cfgi_confd_component_register(void);

typedef struct {
    orcm_cfgi_base_component_t super;
    char *db;
    char *logfile;
    char *mode;
    char *ns;
    FILE *log_stream;
    bool test_mode;
} orcm_cfgi_confd_component_t;

ORCM_DECLSPEC extern orcm_cfgi_confd_component_t mca_orcm_cfgi_confd_component;
ORCM_DECLSPEC extern orcm_cfgi_base_module_t orcm_cfgi_confd_module;

#endif /* CFGI_CONFD_H */

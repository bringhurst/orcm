/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

/**
 * @file
 *
 * Interface into the ORCM Library
 */
#ifndef ORCM_RUNTIME_H
#define ORCM_RUNTIME_H

#include "openrcm.h"

#include "runtime/orcm_globals.h"

BEGIN_C_DECLS

/**
 * Initialize the ORCM library
 *
 * This function should be called exactly once.  This function should
 * be called by every application using the ORCM library.
 *
 */
ORCM_DECLSPEC int orcm_init(orcm_proc_type_t flags);

/**
 * Initialize the utility level of the ORCM library
 *
 * This function s called by tools to setup their cmd
 * line prior to calling openrcm_init - it is protected from
 * multiple invocations
 *
 */
ORCM_DECLSPEC int orcm_init_util(void);

/**
 * Initialize parameters for ORCM.
 */
/* ORCM_DECLSPEC int openrcm_register_params(void); */

/**
 * Finalize the ORCM library. Any function calling \code
 * openrcm_init should call \code openrcm_finalize. 
 *
 */
ORCM_DECLSPEC int orcm_finalize(void);

/*
 * Cleanup signal handlers
 */
ORCM_DECLSPEC void orcm_remove_signal_handlers(void);

END_C_DECLS

#endif /* RUNTIME_H */

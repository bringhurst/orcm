/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

/**
 * @file
 *
 * Interface into the OPENRCM Library
 */
#ifndef ORCM_RUNTIME_H
#define ORCM_RUNTIME_H

#include "openrcm_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include "opal/event/event.h"

#include "orte/util/proc_info.h"

BEGIN_C_DECLS

/* define some process types */
typedef uint8_t orcm_proc_type_t;
#define OPENRCM_MASTER      0x01
#define OPENRCM_TOOL        0x02
#define OPENRCM_APP         0x04
#define OPENRCM_DAEMON      0x08

#define OPENRCM_PROC_IS_MASTER      (ORTE_PROC_HNP & orte_process_info.proc_type)
#define OPENRCM_PROC_IS_TOOL        (ORTE_PROC_TOOL & orte_process_info.proc_type)
#define OPENRCM_PROC_IS_APP         (ORTE_PROC_APP & orte_process_info.proc_type)
#define OPENRCM_PROC_IS_DAEMON      (ORTE_PROC_DAEMON & orte_process_info.proc_type)

/* define some tool command flags */
typedef uint8_t orcm_tool_cmd_t;
#define OPENRCM_TOOL_CMD_T OPAL_UINT8

#define OPENRCM_TOOL_START_CMD  1
#define OPENRCM_TOOL_STOP_CMD   2
#define OPENRCM_TOOL_PS_CMD     3

/* define an object to hold spawn info */
typedef struct {
    opal_object_t super;
    opal_event_t *ev;
    char *cmd;
    int32_t np;
    char *hosts;
    bool constrain;
    bool add_procs;
    bool debug;
} orcm_spawn_event_t;
ORTE_DECLSPEC OBJ_CLASS_DECLARATION(orcm_spawn_event_t);

/* define a openrcm spawn fn */
typedef void (*orcm_spawn_fn_t)(int fd, short event, void *command);

/* a convenience macro for setting up a launch event */
#define ORCM_SPAWN_EVENT(comd, adp, dbg, n, hsts, cnstrn, cbfunc)   \
    do {                                                            \
        orcm_spawn_event_t *mev;                                    \
        struct timeval now;                                         \
        OPAL_OUTPUT_VERBOSE((1, orcm_debug_output,                  \
                            "defining message event: %s:%d",        \
                            __FILE__, __LINE__));                   \
        mev = OBJ_NEW(orcm_spawn_event_t);                          \
        mev->cmd = strdup((comd));                                  \
        mev->np = (n);                                              \
        if ((adp)) {                                                \
            mev->add_procs = true;                                  \
        }                                                           \
        if ((dbg)) {                                                \
            mev->debug = true;                                      \
        }                                                           \
        if (NULL != (hsts)) {                                       \
            mev->hosts = strdup((hsts));                            \
        }                                                           \
        mev->constrain = (0 == cnstrn) ? false : true;              \
        opal_evtimer_set(mev->ev, (cbfunc), mev);                   \
        now.tv_sec = 0;                                             \
        now.tv_usec = 0;                                            \
        opal_evtimer_add(mev->ev, &now);                            \
    } while(0);
    
/** version string of OPENRCM */
ORCM_DECLSPEC extern const char openrcm_version_string[];

/**
 * Whether OPENRCM is initialized or we are in openrcm_finalize
 */
ORCM_DECLSPEC extern bool orcm_initialized;
ORCM_DECLSPEC extern bool orcm_util_initialized;
ORCM_DECLSPEC extern bool orcm_finalizing;

/* debugger output control */
ORCM_DECLSPEC extern int orcm_debug_output;
ORCM_DECLSPEC extern int orcm_debug_verbosity;

/**
 * Initialize the OPENRCM library
 *
 * This function should be called exactly once.  This function should
 * be called by every application using the OPENRCM library.
 *
 */
ORCM_DECLSPEC int orcm_init(orcm_proc_type_t flags);

/**
 * Initialize the utility level of the OPENRCM library
 *
 * This function s called by tools to setup their cmd
 * line prior to calling openrcm_init - it is protected from
 * multiple invocations
 *
 */
ORCM_DECLSPEC int orcm_init_util(void);

/**
 * Initialize parameters for OPENRCM.
 */
/* ORCM_DECLSPEC int openrcm_register_params(void); */

/**
 * Finalize the OPENRCM library. Any function calling \code
 * openrcm_init should call \code openrcm_finalize. 
 *
 */
ORCM_DECLSPEC int orcm_finalize(void);

/**
 * Spawn an application under the CM
 */
ORCM_DECLSPEC int orcm_spawn(int fd, short event, void *command);

END_C_DECLS

#endif /* RUNTIME_H */

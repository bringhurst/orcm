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
typedef orte_proc_type_t orcm_proc_type_t;
#define ORCM_MASTER     (ORTE_PROC_HNP | ORTE_PROC_CM)
#define ORCM_TOOL       (ORTE_PROC_TOOL | ORTE_PROC_CM)
#define ORCM_APP        (ORTE_PROC_NON_MPI | ORTE_PROC_CM)
#define ORCM_DAEMON     (ORTE_PROC_DAEMON | ORTE_PROC_CM)
#define ORCM_IOF_ENDPT  0x1000

#define ORCM_PROC_IS_MASTER     (ORTE_PROC_IS_HNP && ORTE_PROC_IS_CM)
#define ORCM_PROC_IS_TOOL       (ORTE_PROC_IS_TOOL && ORTE_PROC_IS_CM)
#define ORCM_PROC_IS_APP        (ORTE_PROC_IS_NON_MPI && ORTE_PROC_IS_CM)
#define ORCM_PROC_IS_DAEMON     (ORTE_PROC_IS_DAEMON && ORTE_PROC_IS_CM)
#define ORCM_PROC_IS_IOF_ENDPT  (ORCM_IOF_ENDPT & orte_process_info.proc_type)

/* define some tool command flags */
typedef uint8_t orcm_tool_cmd_t;
#define ORCM_TOOL_CMD_T OPAL_UINT8

#define ORCM_TOOL_START_CMD          1
#define ORCM_TOOL_STOP_CMD           2

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
    bool continuous;
    int max_restarts;
} orcm_spawn_event_t;
ORTE_DECLSPEC OBJ_CLASS_DECLARATION(orcm_spawn_event_t);

/* define a openrcm spawn fn */
typedef void (*orcm_spawn_fn_t)(int fd, short event, void *command);

/* a convenience macro for setting up a launch event */
#define ORCM_SPAWN_EVENT(comd, adp, cnt, dbg, rstrts, n, hsts, cnstrn, cbfunc)   \
    do {                                                                    \
        orcm_spawn_event_t *mev;                                            \
        struct timeval now;                                                 \
        OPAL_OUTPUT_VERBOSE((1, orcm_debug_output,                          \
                            "defining message event: %s:%d",                \
                            __FILE__, __LINE__));                           \
        mev = OBJ_NEW(orcm_spawn_event_t);                                  \
        mev->cmd = strdup((comd));                                          \
        mev->np = (n);                                                      \
        if (0 != (adp)) {                                                   \
            mev->add_procs = true;                                          \
        }                                                                   \
        if (0 != (dbg)) {                                                   \
            mev->debug = true;                                              \
        }                                                                   \
        if (0 != (cnt)) {                                                   \
            mev->continuous = true;                                         \
        }                                                                   \
        mev->max_restarts = (rstrts);                                       \
        if (NULL != (hsts)) {                                               \
            mev->hosts = strdup((hsts));                                    \
        }                                                                   \
        mev->constrain = (0 == cnstrn) ? false : true;                      \
        opal_evtimer_set(mev->ev, (cbfunc), mev);                           \
        now.tv_sec = 0;                                                     \
        now.tv_usec = 0;                                                    \
        opal_evtimer_add(mev->ev, &now);                                    \
    } while(0);
    
/** version string of ORCM */
ORCM_DECLSPEC extern const char openrcm_version_string[];

/**
 * Whether ORCM is initialized or we are in openrcm_finalize
 */
ORCM_DECLSPEC extern bool orcm_initialized;
ORCM_DECLSPEC extern bool orcm_util_initialized;
ORCM_DECLSPEC extern bool orcm_finalizing;

/* debugger output control */
ORCM_DECLSPEC extern int orcm_debug_output;
ORCM_DECLSPEC extern int orcm_debug_verbosity;

/* track if I am the lowest rank alive in a job */
ORCM_DECLSPEC extern bool orcm_lowest_rank;

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
void orcm_remove_signal_handlers(void);

/**
 * Spawn an application under the CM
 */
ORCM_DECLSPEC int orcm_spawn(int fd, short event, void *command);

END_C_DECLS

#endif /* RUNTIME_H */

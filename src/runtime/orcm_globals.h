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
#ifndef ORCM_GLOBALS_H
#define ORCM_GLOBALS_H

#include "openrcm.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include "opal/mca/event/event.h"
#include "opal/class/opal_ring_buffer.h"
#include "opal/class/opal_pointer_array.h"

#include "orte/threads/threads.h"
#include "orte/util/proc_info.h"
#include "orte/mca/rmcast/rmcast_types.h"

BEGIN_C_DECLS

#define ORCM_MAX_MSG_RING_SIZE   8

/* define some process types */
typedef orte_proc_type_t orcm_proc_type_t;
#define ORCM_MASTER         (ORTE_PROC_HNP | ORTE_PROC_CM)
#define ORCM_TOOL           (ORTE_PROC_TOOL | ORTE_PROC_CM)
#define ORCM_APP            (ORTE_PROC_NON_MPI | ORTE_PROC_CM)
#define ORCM_DAEMON         (ORTE_PROC_DAEMON | ORTE_PROC_CM)
#define ORCM_IOF_ENDPT      0x1000
#define ORCM_SCHEDULER      0x2000
#define ORCM_DEBUGGER_HOST  0x4000

#define ORCM_PROC_IS_MASTER         (ORTE_PROC_IS_HNP && ORTE_PROC_IS_CM)
#define ORCM_PROC_IS_TOOL           (ORTE_PROC_IS_TOOL && ORTE_PROC_IS_CM)
#define ORCM_PROC_IS_APP            (ORTE_PROC_IS_NON_MPI && ORTE_PROC_IS_CM)
#define ORCM_PROC_IS_DAEMON         (ORTE_PROC_IS_DAEMON && ORTE_PROC_IS_CM)
#define ORCM_PROC_IS_IOF_ENDPT      (ORCM_IOF_ENDPT & orte_process_info.proc_type)
#define ORCM_PROC_IS_SCHEDULER      (ORCM_SCHEDULER & orte_process_info.proc_type)
#define ORCM_PROC_IS_DEBUGGER_HOST  (ORCM_DEBUGGER_HOST & orte_process_info.proc_type)

/* define some tool command flags */
typedef uint8_t orcm_tool_cmd_t;
#define ORCM_TOOL_CMD_T OPAL_UINT8

#define ORCM_TOOL_START_CMD          1
#define ORCM_TOOL_STOP_CMD           2
#define ORCM_TOOL_ILLEGAL_CMD        3

/* define some notify flags */
typedef uint8_t orcm_notify_t;
#define ORCM_NOTIFY_NONE    0x00
#define ORCM_NOTIFY_LDR     0x01
#define ORCM_NOTIFY_GRP     0x02
#define ORCM_NOTIFY_ANY     0x04
#define ORCM_NOTIFY_ALL     0x08

/* pnp type required in global object - see pnp_types.h for value defines */
typedef uint32_t orcm_pnp_channel_t;
#define ORCM_PNP_CHANNEL_T  OPAL_UINT32

/* callback prototypes required at the global level - these
 * are named according to the framework they are associated with
 */
typedef void (*orcm_leader_cbfunc_t)(const char *stringid,
                                     const orte_process_name_t *failed,
                                     const orte_process_name_t *leader);

typedef void (*orcm_pnp_open_channel_cbfunc_t)(const char *app,
                                               const char *version,
                                               const char *release,
                                               orcm_pnp_channel_t channel);

/* global objects - need to be accessed from multiple frameworks */
typedef struct {
    opal_object_t super;
    /* thread protection */
    orte_thread_ctl_t ctl;
    /* storage for wildcard triplets - this is where
     * we "hold" recvs registered against triplets
     * containing one or more wildcard fields
     */
    opal_pointer_array_t wildcards;
    /* storage for triplets */
    opal_pointer_array_t array;
} orcm_triplets_array_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_triplets_array_t);

typedef struct {
    opal_object_t super;
    /* thread protection */
    orte_thread_ctl_t ctl;
    /* id and groups */
    char *string_id;
    orte_vpid_t num_procs;
    opal_pointer_array_t groups;
    /* pnp support */
    opal_list_t input_recvs;
    opal_list_t output_recvs;
    orte_jobid_t pnp_cb_policy;
    orcm_pnp_open_channel_cbfunc_t pnp_cbfunc;
    /* leader support */
    bool leader_set;
    orte_process_name_t leader_policy;
    orte_process_name_t leader;
    orcm_notify_t notify;
    orcm_leader_cbfunc_t leader_cbfunc;
} orcm_triplet_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_triplet_t);

typedef struct {
    opal_object_t super;
    /* identification */
    orcm_triplet_t *triplet;
    uint32_t uid;
    orte_jobid_t jobid;
    orte_vpid_t num_procs;
    /* pnp support */
    orcm_pnp_channel_t input;
    orcm_pnp_channel_t output;
    bool pnp_cb_done;
    orcm_pnp_open_channel_cbfunc_t pnp_cbfunc;
    /* leader support */
    orte_vpid_t leader;
    /* members */
    opal_pointer_array_t members;
} orcm_triplet_group_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_triplet_group_t);

typedef struct {
    opal_object_t super;
    /* thread protection */
    orte_thread_ctl_t ctl;
    /* id */
    orte_process_name_t name;
    /* state */
    bool alive;
} orcm_source_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_source_t);

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

/* storage for values reqd by multiple frameworks */
ORCM_DECLSPEC extern orcm_triplets_array_t *orcm_triplets;
ORCM_DECLSPEC extern int orcm_max_msg_ring_size;
ORCM_DECLSPEC extern orte_process_name_t orcm_default_leader_policy;

#define ORCM_WILDCARD_STRING_ID "@:@:@"

#define ORCM_CREATE_STRING_ID(sid, a, v, r) \
    do {                                        \
        asprintf((sid), "%s:%s:%s",             \
                 (NULL == (a)) ? "@" : (a),     \
                 (NULL == (v)) ? "@" : (v),     \
                 (NULL == (r)) ? "@" : (r));    \
    } while(0);

#define ORCM_DECOMPOSE_STRING_ID(sid, a, v, r)  \
    do {                                            \
        char *c, *c2, *t;                           \
        t = strdup((sid));                          \
        c = strchr(t, ':');                         \
        *c = '\0';                                  \
        if (0 == strcmp(t, "@")) {                  \
            (a) = NULL;                             \
        } else {                                    \
            (a) = strdup(t);                        \
        }                                           \
        c++;                                        \
        c2 = strchr(c, ':');                        \
        *c2 = '\0';                                 \
        if (0 == strcmp(c, "@")) {                  \
            (v) = NULL;                             \
        } else {                                    \
            (v) = strdup(c);                        \
        }                                           \
        c2++;                                       \
        if (0 == strcmp(c2, "@")) {                 \
            (r) = NULL;                             \
        } else {                                    \
            (r) = strdup(c2);                       \
        }                                           \
        free(t);                                    \
    } while(0);


END_C_DECLS

#endif /* ORCM_GLOBALS_H */

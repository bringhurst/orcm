/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef ORCMRUN_ORCMRUN_H
#define ORCMRUN_ORCMRUN_H

#include "orte_config.h"
#include "opal/threads/mutex.h"

BEGIN_C_DECLS

/**
 * Main body of orcmrun functionality
 */
int orcmrun(int argc, char *argv[]);

/**
 * Global struct for catching orcmrun command line options.
 */
struct orcmrun_globals_t {
    bool help;
    bool version;
    bool verbose;
    char *report_pid;
    char *report_uri;
    char *sched;
    char *hnp_uri;
    bool exit;
    bool by_node;
    bool by_slot;
    bool by_board;
    bool by_socket;
    bool bind_to_none;
    bool bind_to_core;
    bool bind_to_board;
    bool bind_to_socket;
    bool debugger;
    int num_procs;
    char *env_val;
    char *appfile;
    char *wdir;
    char *path;
    bool preload_binary;
    char *preload_files;
    char *preload_files_dest_dir;
    opal_mutex_t lock;
    bool sleep;
    char *ompi_server;
    bool wait_for_server;
    int server_wait_timeout;
    char *stdin_target;
#if OPAL_ENABLE_FT_CR == 1
    char *sstore_load;
#endif
    bool disable_recovery;
};

/**
 * Struct holding values gleaned from the orcmrun command line -
 * needed by debugger init
 */
ORTE_DECLSPEC extern struct orcmrun_globals_t orcmrun_globals;

END_C_DECLS

#endif /* ORCMRUN_ORCMRUN_H */

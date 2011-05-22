/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef CFGI_BASE_PRIVATE_H
#define CFGI_BASE_PRIVATE_H

#include "openrcm_config_private.h"
#include "include/constants.h"

#include "opal/class/opal_list.h"
#include "opal/dss/dss_types.h"

#include "orte/runtime/orte_globals.h"

#include "mca/cfgi/cfgi.h"

BEGIN_C_DECLS

#define ORCM_CFGI_SPAWN     0x01
#define ORCM_CFGI_KILL_JOB  0x02
#define ORCM_CFGI_KILL_EXE  0x03

typedef struct {
    opal_object_t super;
    int idx;         /* location in the app's executables array */
    char *appname;
    int32_t process_limit;
    int32_t total_procs;
    opal_pointer_array_t versions;
} orcm_cfgi_exec_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_cfgi_exec_t);

typedef struct {
    opal_object_t super;
    int idx;        /* location in the executable's versions array */
    orcm_cfgi_exec_t *exec;   /* the executable this belongs to */
    char *version;
    char **argv;
    /* keep a printable time of last modification so
     * we can tell when it was last updated
     */
    char *mod_time;
    /* track the executing copies of this version */
    opal_pointer_array_t binaries;
} orcm_cfgi_version_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_cfgi_version_t);

typedef struct {
    opal_object_t super;
    bool modified;
    int idx;       /* location in the installed_apps array */
    char *application;
    int32_t max_instances;
    int32_t num_instances;
    opal_pointer_array_t executables;
    opal_pointer_array_t instances;
} orcm_cfgi_app_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_cfgi_app_t);

typedef struct {
    opal_object_t super;
    int idx;       /* location in the run's binaries array */
    int vers_idx;  /* location in the version's binaries array */
    orcm_cfgi_version_t *vers;  /* version this is based on */
    orcm_cfgi_exec_t *exec;     /* the executable this belongs to */
    /* also store the strings as they may not be defined yet */
    char *version;
    char *appname;
    /* store the resulting binary name */
    char *binary;
    int32_t num_procs;
} orcm_cfgi_bin_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_cfgi_bin_t);

typedef struct {
    opal_object_t super;
    int idx;
    int app_idx;
    orcm_cfgi_app_t *app;  /* app this is based on */
    char *application;     /* store the name in case the app isn't defined yet */
    char *instance;
    opal_pointer_array_t binaries;
} orcm_cfgi_run_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_cfgi_run_t);

typedef struct {
    opal_object_t super;
    uint8_t cmd;
    bool cleanup;
    orcm_cfgi_run_t *run;
    orte_job_t *jdata;
} orcm_cfgi_caddy_t;
ORCM_DECLSPEC OBJ_CLASS_DECLARATION(orcm_cfgi_caddy_t);


#define ORCM_CFGI_EXEC     1
#define ORCM_CFGI_APP      2
#define ORCM_CFGI_VERSION  3
#define ORCM_CFGI_RUN      4
#define ORCM_CFGI_BIN      5
#define ORCM_CFGI_CADDY    6

ORCM_DECLSPEC int orcm_cfgi_base_check_job(orte_job_t *jdat);

ORCM_DECLSPEC void orcm_cfgi_base_dump(char **output, char *pfx, void *ptr, int type);

ORCM_DECLSPEC bool orcm_cfgi_app_definition_valid(orcm_cfgi_app_t *app);


typedef struct {
    opal_list_item_t super;
    int pri;
    orcm_cfgi_base_module_t *module;
} orcm_cfgi_base_selected_module_t;
OBJ_CLASS_DECLARATION(orcm_cfgi_base_selected_module_t);

END_C_DECLS

#endif

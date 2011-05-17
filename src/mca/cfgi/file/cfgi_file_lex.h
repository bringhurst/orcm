/* -*- C -*-
 *
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011      Cisco Systems, Inc. All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef ORCM_CFGI_FILE_LEX_H_
#define ORCM_CFGI_FILE_LEX_H_

#include "openrcm_config_private.h"
#include "include/constants.h"

#include <stdio.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <stdarg.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef malloc
#undef malloc
#endif
#ifdef realloc
#undef realloc
#endif
#ifdef free
#undef free
#endif

typedef union {
    int ival;
    char* sval;
} orcm_cfgi_file_value_t;

extern int   orcm_cfgi_file_lex(void);
extern FILE *orcm_cfgi_file_in;
extern int   orcm_cfgi_file_line;
extern bool  orcm_cfgi_file_done;
extern orcm_cfgi_file_value_t  orcm_cfgi_file_value;

/*
 * Make lex-generated files not issue compiler warnings
 */
#define YY_STACK_USED 0
#define YY_ALWAYS_INTERACTIVE 0
#define YY_NEVER_INTERACTIVE 0
#define YY_MAIN 0
#define YY_NO_UNPUT 1
#define YY_SKIP_YYWRAP 1

/* generic values */
#define ORCM_CFGI_FILE_DONE                   0
#define ORCM_CFGI_FILE_ERROR                  1
#define ORCM_CFGI_FILE_QUOTED_STRING          2
#define ORCM_CFGI_FILE_EQUAL                  3
#define ORCM_CFGI_FILE_INT                    4
#define ORCM_CFGI_FILE_STRING                 5
#define ORCM_CFGI_FILE_NEWLINE                6

/* orcm syntax */
#define ORCM_CFGI_FILE_APPLICATION            7
#define ORCM_CFGI_FILE_MAX_INSTANCES          8
#define ORCM_CFGI_FILE_EXECUTABLE             9
#define ORCM_CFGI_FILE_VERSION               10
#define ORCM_CFGI_FILE_PROCESS_LIMIT         11
#define ORCM_CFGI_FILE_APP_VERSION           12
#define ORCM_CFGI_FILE_ARGV                  13

#endif

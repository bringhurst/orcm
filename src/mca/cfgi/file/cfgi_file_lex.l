%{ /* -*- C -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
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
#include "openrcm_config_private.h"
#include "include/constants.h"

#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "cfgi_file_lex.h"

/*
 * local functions
 */
BEGIN_C_DECLS

int orcm_cfgi_file_wrap(void);

END_C_DECLS

int orcm_cfgi_file_wrap(void)
{
    orcm_cfgi_file_done = true;
    return 1;
}

/*
 * global variables
 */
FILE *orcm_cfgi_file_in;
int orcm_cfgi_file_line=1;
orcm_cfgi_file_value_t orcm_cfgi_file_value;
bool orcm_cfgi_file_done = false;

%}

WHITE       [\f\t\v ]

%x comment

%%

{WHITE}*\n         { orcm_cfgi_file_line++; 
                     return ORCM_CFGI_FILE_NEWLINE; }
#.*\n              { orcm_cfgi_file_line++; 
                     return ORCM_CFGI_FILE_NEWLINE; }
"//".*\n           { orcm_cfgi_file_line++; 
                     return ORCM_CFGI_FILE_NEWLINE; }

"/*"               { BEGIN(comment);
                     return ORCM_CFGI_FILE_NEWLINE; }
<comment>[^*\n]*       ; /* Eat up non '*'s */
<comment>"*"+[^*/\n]*  ; /* Eat '*'s not followed by a '/' */
<comment>\n        { orcm_cfgi_file_line++;
                     return ORCM_CFGI_FILE_NEWLINE; } 
<comment>"*"+"/"   { BEGIN(INITIAL); /* Done with Block Comment */
                     return ORCM_CFGI_FILE_NEWLINE; }

\"[^\"]*\"         { orcm_cfgi_file_value.sval = yytext; 
                     return ORCM_CFGI_FILE_QUOTED_STRING; }

{WHITE}+           ;			/* whitespace */

"="                { return ORCM_CFGI_FILE_EQUAL; }

application        { orcm_cfgi_file_value.sval = yytext; 
                     return ORCM_CFGI_FILE_APPLICATION; }
max-instances      { orcm_cfgi_file_value.sval = yytext; 
                     return ORCM_CFGI_FILE_MAX_INSTANCES; }
max_instances      { orcm_cfgi_file_value.sval = yytext; 
                     return ORCM_CFGI_FILE_MAX_INSTANCES; }
process            { orcm_cfgi_file_value.sval = yytext; 
                     return ORCM_CFGI_FILE_EXECUTABLE; }
version            { orcm_cfgi_file_value.sval = yytext; 
                     return ORCM_CFGI_FILE_VERSION; }
process-limit      { orcm_cfgi_file_value.sval = yytext; 
                     return ORCM_CFGI_FILE_PROCESS_LIMIT; }
process_limit      { orcm_cfgi_file_value.ival = atol(yytext); 
                     return ORCM_CFGI_FILE_PROCESS_LIMIT; }
argv               { orcm_cfgi_file_value.sval = yytext; 
                     return ORCM_CFGI_FILE_ARGV; }

%{ /* Take all else as strings */
%}

[A-za-z0-9_\-\.\:]*  { orcm_cfgi_file_value.sval = yytext;
                     return ORCM_CFGI_FILE_STRING; }


.                  { orcm_cfgi_file_value.sval = yytext; 
                     return ORCM_CFGI_FILE_ERROR; }

%%

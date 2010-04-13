/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef ORCM_CONSTANTS_H
#define ORCM_CONSTANTS_H

#include "openrcm.h"

#include "orte/constants.h"

BEGIN_C_DECLS

#define ORCM_ERR_BASE            ORTE_ERR_MAX


enum {
    /* Error codes inherited from ORTE.  Still enum values so that we
       get the nice debugger help. */

    ORCM_SUCCESS                             = ORTE_SUCCESS,

    ORCM_ERROR                               = ORTE_ERROR,
    ORCM_ERR_OUT_OF_RESOURCE                 = ORTE_ERR_OUT_OF_RESOURCE,
    ORCM_ERR_TEMP_OUT_OF_RESOURCE            = ORTE_ERR_TEMP_OUT_OF_RESOURCE,
    ORCM_ERR_RESOURCE_BUSY                   = ORTE_ERR_RESOURCE_BUSY,
    ORCM_ERR_BAD_PARAM                       = ORTE_ERR_BAD_PARAM,
    ORCM_ERR_FATAL                           = ORTE_ERR_FATAL,
    ORCM_ERR_NOT_IMPLEMENTED                 = ORTE_ERR_NOT_IMPLEMENTED,
    ORCM_ERR_NOT_SUPPORTED                   = ORTE_ERR_NOT_SUPPORTED,
    ORCM_ERR_INTERUPTED                      = ORTE_ERR_INTERUPTED,
    ORCM_ERR_WOULD_BLOCK                     = ORTE_ERR_WOULD_BLOCK,
    ORCM_ERR_IN_ERRNO                        = ORTE_ERR_IN_ERRNO,
    ORCM_ERR_UNREACH                         = ORTE_ERR_UNREACH,
    ORCM_ERR_NOT_FOUND                       = ORTE_ERR_NOT_FOUND,
    ORCM_EXISTS                              = ORTE_EXISTS,
    ORCM_ERR_TIMEOUT                         = ORTE_ERR_TIMEOUT,
    ORCM_ERR_NOT_AVAILABLE                   = ORTE_ERR_NOT_AVAILABLE,
    ORCM_ERR_PERM                            = ORTE_ERR_PERM,
    ORCM_ERR_VALUE_OUT_OF_BOUNDS             = ORTE_ERR_VALUE_OUT_OF_BOUNDS,
    ORCM_ERR_FILE_READ_FAILURE               = ORTE_ERR_FILE_READ_FAILURE,
    ORCM_ERR_FILE_WRITE_FAILURE              = ORTE_ERR_FILE_WRITE_FAILURE,
    ORCM_ERR_FILE_OPEN_FAILURE               = ORTE_ERR_FILE_OPEN_FAILURE,
    ORCM_ERR_PACK_MISMATCH                   = ORTE_ERR_PACK_MISMATCH,
    ORCM_ERR_PACK_FAILURE                    = ORTE_ERR_PACK_FAILURE,
    ORCM_ERR_UNPACK_FAILURE                  = ORTE_ERR_UNPACK_FAILURE,
    ORCM_ERR_UNPACK_INADEQUATE_SPACE         = ORTE_ERR_UNPACK_INADEQUATE_SPACE,
    ORCM_ERR_UNPACK_READ_PAST_END_OF_BUFFER  = ORTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER,
    ORCM_ERR_TYPE_MISMATCH                   = ORTE_ERR_TYPE_MISMATCH,
    ORCM_ERR_OPERATION_UNSUPPORTED           = ORTE_ERR_OPERATION_UNSUPPORTED, 
    ORCM_ERR_UNKNOWN_DATA_TYPE               = ORTE_ERR_UNKNOWN_DATA_TYPE,
    ORCM_ERR_BUFFER                          = ORTE_ERR_BUFFER,
    ORCM_ERR_DATA_TYPE_REDEF                 = ORTE_ERR_DATA_TYPE_REDEF,
    ORCM_ERR_DATA_OVERWRITE_ATTEMPT          = ORTE_ERR_DATA_OVERWRITE_ATTEMPT,
    ORCM_ERR_RECV_LESS_THAN_POSTED           = ORTE_ERR_RECV_LESS_THAN_POSTED,
    ORCM_ERR_RECV_MORE_THAN_POSTED           = ORTE_ERR_RECV_MORE_THAN_POSTED,
    ORCM_ERR_NO_MATCH_YET                    = ORTE_ERR_NO_MATCH_YET,
    ORCM_ERR_REQUEST                         = ORTE_ERR_REQUEST,
    ORCM_ERR_NO_CONNECTION_ALLOWED           = ORTE_ERR_NO_CONNECTION_ALLOWED,
    ORCM_ERR_CONNECTION_REFUSED              = ORTE_ERR_CONNECTION_REFUSED,
    ORCM_ERR_CONNECTION_FAILED               = ORTE_ERR_CONNECTION_FAILED,
    ORCM_ERR_COMM_FAILURE                    = ORTE_ERR_COMM_FAILURE,
    ORCM_ERR_COMPARE_FAILURE                 = ORTE_ERR_COMPARE_FAILURE,
    ORCM_ERR_COPY_FAILURE                    = ORTE_ERR_COPY_FAILURE,
    ORCM_ERR_PROC_STATE_MISSING              = ORTE_ERR_PROC_STATE_MISSING,
    ORCM_ERR_PROC_EXIT_STATUS_MISSING        = ORTE_ERR_PROC_EXIT_STATUS_MISSING,
    ORCM_ERR_INDETERMINATE_STATE_INFO        = ORTE_ERR_INDETERMINATE_STATE_INFO,
    ORCM_ERR_NODE_FULLY_USED                 = ORTE_ERR_NODE_FULLY_USED,
    ORCM_ERR_INVALID_NUM_PROCS               = ORTE_ERR_INVALID_NUM_PROCS,
    ORCM_ERR_SILENT                          = ORTE_ERR_SILENT,
    ORCM_ERR_ADDRESSEE_UNKNOWN               = ORTE_ERR_ADDRESSEE_UNKNOWN,
    ORCM_ERR_SYS_LIMITS_PIPES                = ORTE_ERR_SYS_LIMITS_PIPES,
    ORCM_ERR_PIPE_SETUP_FAILURE              = ORTE_ERR_PIPE_SETUP_FAILURE,
    ORCM_ERR_SYS_LIMITS_CHILDREN             = ORTE_ERR_SYS_LIMITS_CHILDREN,
    ORCM_ERR_FAILED_GET_TERM_ATTRS           = ORTE_ERR_FAILED_GET_TERM_ATTRS,
    ORCM_ERR_WDIR_NOT_FOUND                  = ORTE_ERR_WDIR_NOT_FOUND,
    ORCM_ERR_EXE_NOT_FOUND                   = ORTE_ERR_EXE_NOT_FOUND,
    ORCM_ERR_PIPE_READ_FAILURE               = ORTE_ERR_PIPE_READ_FAILURE,
    ORCM_ERR_EXE_NOT_ACCESSIBLE              = ORTE_ERR_EXE_NOT_ACCESSIBLE,
    ORCM_ERR_FAILED_TO_START                 = ORTE_ERR_FAILED_TO_START,
    ORCM_ERR_FILE_NOT_EXECUTABLE             = ORTE_ERR_FILE_NOT_EXECUTABLE,
    ORCM_ERR_HNP_COULD_NOT_START             = ORTE_ERR_HNP_COULD_NOT_START,
    ORCM_ERR_SYS_LIMITS_SOCKETS              = ORTE_ERR_SYS_LIMITS_SOCKETS,
    ORCM_ERR_SOCKET_NOT_AVAILABLE            = ORTE_ERR_SOCKET_NOT_AVAILABLE,
    ORCM_ERR_SYSTEM_WILL_BOOTSTRAP           = ORTE_ERR_SYSTEM_WILL_BOOTSTRAP
};

enum {
    /* error codes specific to ORCM - don't forget to update
     src/util/error_strings.c when adding new error codes!!
     Otherwise, the error reporting system will potentially crash,
     or at the least not be able to report the new error correctly.
     */
    ORCM_ERR_PLACEHOLDER                     = (ORCM_ERR_BASE - 1)
};

#define ORCM_ERR_MAX                      (ORCM_ERR_BASE - 100)

/* include the prototype for the error-to-string converter */
ORCM_DECLSPEC const char* orcm_err2str(int errnum);

END_C_DECLS

#endif /* ORCM_CONSTANTS_H */


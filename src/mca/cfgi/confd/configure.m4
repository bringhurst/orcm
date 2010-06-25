# -*- shell-script -*-
#
# Copyright (c) 2010 Cisco Systems, Inc.  All rights reserved.
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

# MCA_orcm_cfgi_confd_CONFIG([action-if-can-compile], 
#                            [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_orcm_cfgi_confd_CONFIG],[
    # if we don't want specifically ask for
    # this module, don't compile this component
    AC_ARG_ENABLE([confd],
        [AC_HELP_STRING([--enable-confd],
                        [Enable confd integration (default: disabled)])])

    AS_IF([test "$enable_confd" = "yes"],
        [$1], [$2])
])

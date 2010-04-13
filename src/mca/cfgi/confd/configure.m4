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
	AC_CHECK_HEADERS([confd.h], [$1], [$2])
])

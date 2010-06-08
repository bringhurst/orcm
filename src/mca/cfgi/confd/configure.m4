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
    AC_ARG_WITH([confd], 
        [AC_HELP_STRING([--with-confd(=DIR)],
             [Build confd support, optionally adding DIR/include, DIR/lib, and DIR/lib64 to the search path for headers and libraries])])
    ORCM_CHECK_WITHDIR([confd], [$with_confd], [include/confd.h])
    AC_ARG_WITH([confd-libdir], 
        [AC_HELP_STRING([--with-confd-libdir=DIR],
                        [Search for confd libraries in DIR])])
    ORCM_CHECK_WITHDIR([confd-libdir], [$with_confd_libdir], [libconfd.*])

    AS_IF([test ! -z "$with_confd" -a "$with_confd" != "yes"],
          [orcm_check_confd_dir="$with_confd"])
    AS_IF([test ! -z "$with_confd_libdir" -a "$with_confd_libdir" != "yes"],
          [orcm_check_confd_libdir="$with_confd_libdir"])
    AS_IF([test "$with_confd" = "no"],
          [orcm_check_confd_happy="no"],
          [orcm_check_confd_happy="yes"])

    AS_IF([test "$orcm_check_confd_happy" = "yes"],
          [ORCM_CHECK_PACKAGE([orcm_cfgi_confd],
                              [confd.h],
                              [confd],
                              [confd_init_daemon],
                              [],
                              [$orcm_check_confd_dir],
                              [$orcm_check_confd_libdir],
                              [orcm_check_confd_happy=yes],
                              [orcm_check_confd_happy=no])])

    AC_SUBST(orcm_cfgi_confd_CPPFLAGS)
    AC_SUBST(orcm_cfgi_confd_LDFLAGS)
    AC_SUBST(orcm_cfgi_confd_LIBS)

    AS_IF([test "$orcm_check_confd_happy" = "yes"],
          [$1],
          [AS_IF([test ! -z "$with_confd" -a "$with_confd" != "no"],
                 [AC_MSG_WARN([confd support requested (via --with-confd) but not found.])
                  AC_MSG_ERROR([Aborting.])])
           $2])
])

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
    AC_ARG_WITH([confd],
        [AC_HELP_STRING([--with-confd],
                        [Build confd integration (default: no)])])

    AC_ARG_WITH([confd-libdir],
       [AC_HELP_STRING([--with-confd-libdir=DIR], [Where to search for confd libs])])
    AC_ARG_ENABLE([internal-confd-support],
        [AC_HELP_STRING([--enable-internal-confd-support],
                        [Use the internal ORCM confd support functions (default: no)])])

    AS_IF([test "$with_confd" != ""],[
        ORCM_CHECK_WITHDIR([confd], [$with_confd], [include/confd.h])
        ORCM_CHECK_WITHDIR([confd-libdir], [$with_confd_libdir], [libconfd.*])

        # Defaults
        orcm_check_confd_dir_msg="compiler default"
        orcm_check_confd_libdir_msg="linker default"

        # Save directory names if supplied
        AS_IF([test ! -z "$with_confd" -a "$with_confd" != "yes"],
              [orcm_check_confd_dir="$with_confd"
               orcm_check_confd_dir_msg="$orcm_check_confd_dir (from --with-confd)"])
        AS_IF([test ! -z "$with_confd_libdir" -a "$with_confd_libdir" != "yes"],
              [orcm_check_confd_libdir="$with_confd_libdir"
               orcm_check_confd_libdir_msg="$orcm_check_confd_libdir (from --with-confd-libdir)"])

        # If no directories were specified, look for CONFD_LIBDIR,
        # CONFD_INCLUDEDIR, and/or CONFD_ENVDIR.
        AS_IF([test -z "$orcm_check_confd_dir" -a -z "$orcm_check_confd_libdir"],
              [AS_IF([test ! -z "$CONFD_ENVDIR" -a -z "$CONFD_LIBDIR" -a -f "$CONFD_ENVDIR/confd.conf"],
                     [CONFD_LIBDIR=`egrep ^CONFD_LIBDIR= $CONFD_ENVDIR/confd.conf | cut -d= -f2-`])
               AS_IF([test ! -z "$CONFD_ENVDIR" -a -z "$CONFD_INCLUDEDIR" -a -f "$CONFD_ENVDIR/confd.conf"],
                     [CONFD_INCLUDEDIR=`egrep ^CONFD_INCLUDEDIR= $CONFD_ENVDIR/confd.conf | cut -d= -f2-`])
               AS_IF([test ! -z "$CONFD_LIBDIR"],
                     [orcm_check_confd_libdir=$CONFD_LIBDIR
                      orcm_check_confd_libdir_msg="$CONFD_LIBDIR (from \$CONFD_LIBDIR)"])
               AS_IF([test ! -z "$CONFD_INCLUDEDIR"],
                     [orcm_check_confd_dir=`dirname $CONFD_INCLUDEDIR`
                      orcm_check_confd_dir_msg="$orcm_check_confd_dir (from \$CONFD_INCLUDEDIR)"])])

        AS_IF([test "$with_confd" = "no"],
              [orcm_check_confd_happy="no"],
              [orcm_check_confd_happy="yes"])

        orcm_check_confd_save_CPPFLAGS="$CPPFLAGS"
        orcm_check_confd_save_LDFLAGS="$LDFLAGS"
        orcm_check_confd_save_LIBS="$LIBS"

        AS_IF([test "$orcm_check_confd_happy" = "yes"], 
              [AC_MSG_CHECKING([for CONFD dir])
               AC_MSG_RESULT([$orcm_check_confd_dir_msg])
               AC_MSG_CHECKING([for CONFD library dir])
               AC_MSG_RESULT([$orcm_check_confd_libdir_msg])
               ORCM_CHECK_PACKAGE([orcm_cfgi_confd],
                                  [confd.h],
                                  [confd],
                                  [confd_lasterr],
                                  [],
                                  [$orcm_check_confd_dir],
                                  [$orcm_check_confd_libdir],
                                  [orcm_check_confd_happy="yes"],
                                  [orcm_check_confd_happy="no"])])

        AC_SUBST(orcm_cfgi_confd_CPPFLAGS)
        AC_SUBST(orcm_cfgi_confd_LDFLAGS)
        AC_SUBST(orcm_cfgi_confd_LIBS)

        CPPFLAGS="$orcm_check_confd_save_CPPFLAGS"
        LDFLAGS="$orcm_check_confd_save_LDFLAGS"
        LIBS="$orcm_check_confd_save_LIBS"

        # see if internal support functions are needed
        AC_MSG_CHECKING([for internal confd support])
        AM_CONDITIONAL([WANT_LOCAL_CONFD], test "$enable_internal_confd_support" = "yes")
        if test "$enable_internal_confd_support" = "yes"; then
            want_local_confd_flag=1
            AC_MSG_RESULT([yes])
        else
            AC_MSG_RESULT([no])
            want_local_confd_flag=0
        fi
        AC_DEFINE_UNQUOTED([WANT_LOCAL_CONFD], [$want_local_confd_flag], [Use local confd support])

        AS_IF([test "$orcm_check_confd_happy" = "yes"],
              [$1],
              [AS_IF([test ! -z "$with_confd" -a "$with_confd" != "no"],
                     [AC_MSG_WARN([CONFD support requested (via --with-confd) but not found.])
                      AC_MSG_ERROR([Aborting.])])
               $2])
    ],[
        # see if internal support functions are needed
        AM_CONDITIONAL([WANT_LOCAL_CONFD], 0)
        $2
    ])


])


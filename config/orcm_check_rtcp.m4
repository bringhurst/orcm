#
# RTCP support
#
AC_DEF_FUNC([ORCM_CHECK_RTCP],[
    orcm_want_rtcp=no
    AC_MSG_CHECKING([if want rtcp support])
    AC_ARG_WITH([rtcp],
                [AC_HELP_STRING([--with-rtcp],
                [Build with rtcp support from the specified directory])])
    AC_ARG_WITH([rtcp-libdir],
                [AC_HELP_STRING([--with-rtcp-libdir],
		["Search for rtcp libraries in DIR"])])
    if test "$with_rtcp" = "no"; then
        orcm_want_rtcp=no
        AM_CONDITIONAL(ORCM_WANT_RTCP, 0)
        AC_MSG_RESULT([no])
    elif test -z "$with_rtcp"; then
        orcm_want_rtcp=no
        AM_CONDITIONAL(ORCM_WANT_RTCP, 0)
        AC_MSG_RESULT([no])
    else
        AC_MSG_RESULT([yes])
        ORCM_CHECK_WITHDIR([rtcp], [$with_rtcp], [include/rtcp.h])
        ORCM_CHECK_WITHDIR([rtcp-libdir], [$with_rtcp_libdir], [librtcp.*])

        # Defaults
        orcm_check_rtcp_dir_msg="compiler default"
        orcm_check_rtcp_libdir_msg="linker default"

        # Save directory names if supplied
        AS_IF(["$with_rtcp" != "yes"],
              [orcm_check_rtcp_dir="$with_rtcp"
               orcm_check_rtcp_dir_msg="$orcm_check_rtcp_dir (from --with-rtcp)"])
        AS_IF([test ! -z "$with_rtcp_libdir" -a "$with_rtcp_libdir" != "yes"],
              [orcm_check_rtcp_libdir="$with_rtcp_libdir"
               orcm_check_rtcp_libdir_msg="$orcm_check_rtcp_libdir (from --with-rtcp-libdir)"])

        # If no directories were specified, look for RTCP_LIBDIR,
        # RTCP_INCLUDEDIR, and/or RTCP_ENVDIR.
        AS_IF([test -z "$orcm_check_rtcp_dir" -a -z "$orcm_check_rtcp_libdir"],
              [AS_IF([test ! -z "$RTCP_ENVDIR" -a -z "$RTCP_LIBDIR" -a -f "$RTCP_ENVDIR/rtcp.conf"],
                     [RTCP_LIBDIR=`egrep ^RTCP_LIBDIR= $RTCP_ENVDIR/rtcp.conf | cut -d= -f2-`])
               AS_IF([test ! -z "$RTCP_ENVDIR" -a -z "$RTCP_INCLUDEDIR" -a -f "$RTCP_ENVDIR/rtcp.conf"],
                     [RTCP_INCLUDEDIR=`egrep ^RTCP_INCLUDEDIR= $RTCP_ENVDIR/rtcp.conf | cut -d= -f2-`])
               AS_IF([test ! -z "$RTCP_LIBDIR"],
                     [orcm_check_rtcp_libdir=$RTCP_LIBDIR
                      orcm_check_rtcp_libdir_msg="$RTCP_LIBDIR (from \$RTCP_LIBDIR)"])
               AS_IF([test ! -z "$RTCP_INCLUDEDIR"],
                     [orcm_check_rtcp_dir=`dirname $RTCP_INCLUDEDIR`
                      orcm_check_rtcp_dir_msg="$orcm_check_rtcp_dir (from \$RTCP_INCLUDEDIR)"])])


        AC_MSG_CHECKING([for RTCP dir])
        AC_MSG_RESULT([$orcm_check_rtcp_dir_msg])

        AC_MSG_CHECKING([for RTCP library dir])
        AC_MSG_RESULT([$orcm_check_rtcp_libdir_msg])
        ORCM_CHECK_PACKAGE([rtcp],
                           [rtcp.h],
                           [confd],
                           [confd_lasterr],
                           [],
                           [$orcm_check_rtcp_dir],
                           [$orcm_check_rtcp_libdir],
                           [orcm_want_rtcp="yes"],
                           [orcm_want_rtcp="no"])

        AS_IF([test "$orcm_want_rtcp" = "yes"],
              [AM_CONDITIONAL(ORCM_WANT_RTCP, 1)
               CPPFLAGS="$rtcp_CPPFLAGS $CPPFLAGS"
               LDFLAGS="$rtcp_LDFLAGS $LDFLAGS"
               LIBS="$rtcp_LIBS $LIBS"],
              [AC_MSG_WARN([RTCP support requested (via --with-rtcp) but not found.])
               AC_MSG_ERROR([Aborting.])])
    fi

])dnl

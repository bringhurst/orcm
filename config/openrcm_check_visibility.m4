# -*- shell-script -*-
#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2007 High Performance Computing Center Stuttgart, 
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2006-2011 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2009      Sun Microsystems, Inc.  All rights reserved.
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

# ORCM_CHECK_VISIBILITY
# --------------------------------------------------------
AC_DEFUN([ORCM_CHECK_VISIBILITY],[
    AC_REQUIRE([AC_PROG_GREP])

    msg="whether to enable symbol visibility"
    # Check if the compiler has support for visibility, like some
    # versions of gcc, icc cc.
    AC_ARG_ENABLE(visibility, 
        AC_HELP_STRING([--enable-visibility],
            [enable visibility feature of certain compilers/linkers (default: enabled)]))
    if test "$enable_visibility" = "no"; then
        AC_MSG_CHECKING([$msg])
        AC_MSG_RESULT([no (disabled)]) 
        have_visibility=0
    else
        CFLAGS_orig="$CFLAGS"
        add=

        # check using gcc -fvisibility=hidden flag
        CFLAGS="$CFLAGS_orig -fvisibility=hidden"
        AC_CACHE_CHECK([if $CC supports -fvisibility],
            [orcm_cv_cc_fvisibility],
            [AC_LINK_IFELSE([AC_LANG_PROGRAM([[
                    __attribute__((visibility("default"))) int foo;
                    ]],[[int i;]])],
                    [orcm_cv_cc_fvisibility=yes],
                    [if test -s conftest.err ; then
                        $GREP -iq "visibility" conftest.err
                        if test "$?" = "0" ; then
                            orcm_cv_cc_fvisibility=no
                        else
                            orcm_cv_cc_fvisibility=yes
                        fi
                     else
                        orcm_cv_cc_fvisibility=yes
                     fi],
                    [orcm_cv_cc_fvisibility=no])
                ])
        if test "$orcm_cv_cc_fvisibility" = "yes" ; then
            add=" -fvisibility=hidden"
            have_visibility=1
            AC_MSG_CHECKING([$msg])
            AC_MSG_RESULT([yes (via$add)]) 
            AC_MSG_WARN([$add has been added to CFLAGS])
        elif test "$orcm_cv_cc_xldscope" = "yes" ; then
            add=" -xldscope=hidden"
            have_visibility=1
            AC_MSG_CHECKING([$msg])
            AC_MSG_RESULT([yes (via$add)]) 
            AC_MSG_WARN([$add has been added to CFLAGS])
        elif test "$enable_visibility" = "yes"; then
            AC_MSG_ERROR([Symbol visibility support requested but compiler does not seem to support it.  Aborting])
        else 
            AC_MSG_CHECKING([$msg])
            AC_MSG_RESULT([no (unsupported)]) 
            have_visibility=0
        fi
        CFLAGS="$CFLAGS_orig$add"
        ORCM_VISIBILITY_CFLAGS="$add"
        unset add 
    fi

    AC_DEFINE_UNQUOTED([ORCM_C_HAVE_VISIBILITY], [$have_visibility],
            [Whether C compiler supports -fvisibility])
])

# -*- shell-script -*-
#
# Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

# OMPI_CHECK_CLIB(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
# check if clib (Eliots programming library) support can be found.
# sets prefix_{CPPFLAGS, LDFLAGS, LIBS} as needed and runs action-if-found if there is
# support, otherwise executes action-if-not-found
AC_DEFUN([OMPI_CHECK_CLIB],[
    AC_ARG_WITH([clib],
        [AC_HELP_STRING([--with-clib(=DIR)],
                [Build CLIB (Eliots programming library) support, searching for libraries in DIR])])
    AC_ARG_WITH([clib-libdir],
        [AC_HELP_STRING([--with-clib-libdir=DIR],
                [Search for CLIB (Eliots programming library) libraries in DIR])])
    
    AS_IF([test "$with_clib" != "no"],
        [AS_IF([test ! -z "$with_clib" -a "$with_clib" != "yes"],
                [ompi_check_clib_dir="$with_clib"])
            AS_IF([test ! -z "$with_clib_libdir" -a "$with_clib_libdir" != "yes"],
                [ompi_check_clib_libdir="$with_clib_libdir"])
            
            OMPI_CHECK_PACKAGE([$1],
                [clib/error.h],
                [clib],
                [clib_error_free_vector],
                ,
                [$ompi_check_clib_dir],
                [$ompi_check_clib_libdir],
                [ompi_check_clib_happy="yes"],
                [ompi_check_clib_happy="no"])
            ],
        [ompi_check_clib_happy="no"])
    
    AS_IF([test "$ompi_check_clib_happy" = "yes"],
        [$2],
        [AS_IF([test ! -z "$with_clib" -a "$with_clib" != "no"],
                [AC_MSG_ERROR([CLIB (Eliots programming library) support requested but not found.  Aborting])])
            $3])
    ])


# OMPI_CHECK_CSM(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
# check if csm (Cisco Shared Memory) support can be found.
# sets prefix_{CPPFLAGS, LDFLAGS, LIBS} as needed and runs action-if-found if there is
# support, otherwise executes action-if-not-found
AC_DEFUN([OMPI_CHECK_CSM],[
    AC_ARG_WITH([clib],
        [AC_HELP_STRING([--with-clib(=DIR)],
                [Build CLIB (Eliots programming library) support, searching for libraries in DIR])])
    AC_ARG_WITH([clib-libdir],
        [AC_HELP_STRING([--with-clib-libdir=DIR],
                [Search for CLIB (Eliots programming library) libraries in DIR])])
    AC_ARG_WITH([csm],
        [AC_HELP_STRING([--with-csm(=DIR)],
                [Build Cisco Shared Memory support, searching for libraries in DIR])])
    AC_ARG_WITH([csm-libdir],
        [AC_HELP_STRING([--with-csm-libdir=DIR],
                [Search for Cisco Shared Memory libraries in DIR])])
    
    AS_IF([test "$with_csm" != "no"],
        [AS_IF([test ! -z "$with_clib" -a "$with_clib" != "yes"],
                [ompi_check_clib_dir="$with_clib"])
            AS_IF([test ! -z "$with_clib_libdir" -a "$with_clib_libdir" != "yes"],
                [ompi_check_clib_libdir="$with_clib_libdir"])
            AS_IF([test ! -z "$with_csm" -a "$with_csm" != "yes"],
                [ompi_check_csm_dir="$with_csm"])
            AS_IF([test ! -z "$with_csm_libdir" -a "$with_csm_libdir" != "yes"],
                [ompi_check_csm_libdir="$with_csm_libdir"])

            ompi_check_csm_$1_save_CPPFLAGS="$CPPFLAGS"
            ompi_check_csm_$1_save_LDFLAGS="$LDFLAGS"
            ompi_check_csm_$1_save_LIBS="$LIBS"

            OMPI_CHECK_PACKAGE([$1],
                [clib/error.h],
                [clib],
                [clib_error_free_vector],
                [-lpthread],
                [$ompi_check_clib_dir],
                [$ompi_check_clib_libdir],
                [OMPI_CHECK_PACKAGE([$1],
                    [svmdb.h],
                    [svmdb],
                    [svmdb_map],
                    [-lsvm -lclib -lpthread -lrt],
                    [$ompi_check_csm_dir],
                    [$ompi_check_csm_libdir],
                    [ompi_check_csm_happy="yes"
                        ompi_check_clib_happy="yes"])],
                [ompi_check_csm_happy="no"
                    ompi_check_clib_happy="no"])

            CPPFLAGS="$ompi_check_csm_$1_save_CPPFLAGS"
            LDFLAGS="$ompi_check_csm_$1_save_LDFLAGS"
            LIBS="$ompi_check_csm_$1_save_LIBS"
            ],
        [ompi_check_csm_happy="no"])
    
    AS_IF([test "$ompi_check_csm_happy" = "yes"],
        [$2],
        [AS_IF([test ! -z "$with_csm" -a "$with_csm" != "no"],
            [AS_IF([test "$ompi_check_clib_happy" = "yes"],
                [AC_MSG_ERROR([Cisco Shared Memory support requested but not found.  Aborting])],
                [AC_MSG_ERROR([CLIB (Eliots programming library) support required but not found.  Aborting])]
            )])
            $3])
    ])


# MCA_sensor_csm_CONFIG([action-if-found], [action-if-not-found])
# -----------------------------------------------------------
AC_DEFUN([MCA_sensor_csm_CONFIG], [
    OMPI_CHECK_CSM([sensor_csm],
                     [sensor_csm_happy="yes"],
                     [sensor_csm_happy="no"])

    AS_IF([test "$sensor_csm_happy" = "yes"],
          [sensor_csm_WRAPPER_EXTRA_LDFLAGS="$sensor_csm_LDFLAGS"
           sensor_csm_WRAPPER_EXTRA_LIBS="$sensor_csm_LIBS"
           $1],
          [$2])

    # substitute in the things needed to build csm
    AC_SUBST([sensor_csm_CFLAGS])
    AC_SUBST([sensor_csm_CPPFLAGS])
    AC_SUBST([sensor_csm_LDFLAGS])
    AC_SUBST([sensor_csm_LIBS])
])dnl

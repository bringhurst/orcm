# -*- autoconf -*-
#
# Copyright © 2009-2010 Cisco Systems, Inc.  All rights reserved.
#

#
# Initialize autostuff
#

AC_PREREQ([2.63])
AC_INIT([openrcm], 
        [m4_normalize(esyscmd([config/openrcm_get_version.sh VERSION --base]))],
        [rcastain@cisco.com], [openrcm])
AC_CONFIG_AUX_DIR([config])
AC_CONFIG_MACRO_DIR([config])

AC_CANONICAL_SYSTEM

AM_INIT_AUTOMAKE([1.10 no-define foreign -Wall -Werror])

# If Automake supports silent rules, enable them.
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

# Make configure depend on the VERSION file, since it's used in AC_INIT

AC_SUBST([CONFIGURE_DEPENDENCIES], ['$(top_srcdir)/VERSION'])

# Get the version of openrcm that we are installing

AC_MSG_CHECKING([for openrcm version])
OPENRCM_VERSION="`$srcdir/config/openrcm_get_version.sh $srcdir/VERSION`"
OPENRCM_MAJOR_VERSION="`$srcdir/config/openrcm_get_version.sh $srcdir/VERSION --major`"
OPENRCM_MINOR_VERSION="`$srcdir/config/openrcm_get_version.sh $srcdir/VERSION --minor`"
OPENRCM_RELEASE_VERSION="`$srcdir/config/openrcm_get_version.sh $srcdir/VERSION --release`"
OPENRCM_SVN_R="`$srcdir/config/openrcm_get_version.sh $srcdir/VERSION --svn`"
AC_SUBST(OPENRCM_VERSION)
AC_SUBST(OPENRCM_SVN_R)
AC_DEFINE_UNQUOTED([OPENRCM_MAJOR_VERSION], [$OPENRCM_MAJOR_VERSION],
    [Major version of openrcm])
AC_DEFINE_UNQUOTED([OPENRCM_MINOR_VERSION], [$OPENRCM_MINOR_VERSION],
    [Minor version of openrcm])
AC_DEFINE_UNQUOTED([OPENRCM_RELEASE_VERSION], [$OPENRCM_RELEASE_VERSION],
    [Release version of openrcm])
AC_MSG_RESULT([$OPENRCM_VERSION])

#
# Do we want to forgive all configure sins and just ensure to make a
# "make dist"able package?
#
AC_ARG_ENABLE([dist],
  [AC_HELP_STRING([--enable-dist],
                  [Guarantee that that the "dist" make target will be functional, although may not guarantee that any other make target will be functional.])],
    ORCM_WANT_DIST=yes, ORCM_WANT_DIST=no)

if test "$ORCM_WANT_DIST" = "yes"; then
    AC_MSG_WARN([*** Configuring in 'make dist' mode])
    AC_MSG_WARN([*** Most make targets may be non-functional!])
fi

#
# Obtain the location of the ORTE installation
#
AC_MSG_CHECKING([where ORTE installation is located])
AC_ARG_WITH([orte],
    [AC_HELP_STRING([--with-orte],
                    [Build with ORTE support from the specified install directory])])
AS_IF([test -z "$with_orte" -a "$ORCM_WANT_DIST" != "yes"],
   [AC_MSG_RESULT([unknown])
    AC_MSG_WARN([*** Must specify --with-orte value])
    AC_MSG_WARN([*** See "configure --help" output])
    AC_MSG_ERROR([Cannot continue])])
AC_MSG_RESULT([found $with_orte])
ORTE_INSTALL_PREFIX="$with_orte"
AC_SUBST(ORTE_INSTALL_PREFIX)

AC_MSG_CHECKING([for ORTE CPPFLAGS])
ORTE_CPPFLAGS="-I$with_orte/include/openmpi"
AC_MSG_RESULT([$ORTE_CPPFLAGS])
AC_MSG_CHECKING([for ORTE LDFLAGS])
ORTE_LDFLAGS="-L$with_orte/lib"
AC_MSG_RESULT([$ORTE_LDFLAGS])

#
# Do we want to use script wrapper compilers
#
AC_ARG_ENABLE([script-wrapper-compilers],
  [AC_HELP_STRING([--enable-script-wrapper-compilers],
     [Use less featured script-based wrapper compilers instead of the standard C-based wrapper compilers.  This option is mainly useful in cross-compile environments])])
AM_CONDITIONAL([OPENRCM_WANT_SCRIPT_WRAPPER_COMPILERS],
    [test "$enable_script_wrapper_compilers" = "yes"])

# Define the location of the OPENRCM prefix
AC_DEFINE_UNQUOTED([OPENRCM_PREFIX], ["$prefix"], [Where openrcm is installed])

# Define the location of the OPENRCM helpfiles
AC_DEFINE_UNQUOTED([OPENRCM_HELPFILES], ["$prefix/share/openrcm"], [Where openrcm helpfiles are installed])

#
# Developer picky compiler options
#
AC_MSG_CHECKING([if want developer-level compiler pickyness])
AC_ARG_ENABLE(picky, 
    AC_HELP_STRING([--enable-picky],
                   [enable developer-level compiler pickyness when building (default: enabled)]))
if test "$enable_picky" = "no"; then
    AC_MSG_RESULT([no])
    WANT_PICKY_COMPILER=0
else
    AC_MSG_RESULT([yes])
    WANT_PICKY_COMPILER=1
fi

#
# Developer debugging
#
AC_MSG_CHECKING([if want developer-level debugging code])
AC_ARG_ENABLE(debug, 
    AC_HELP_STRING([--enable-debug],
                   [enable developer-level debugging code (default: enabled)]))
if test "$enable_debug" = "no"; then
    AC_MSG_RESULT([no])
    WANT_DEBUG=0
else
    AC_MSG_RESULT([yes])
    WANT_DEBUG=1
fi
if test "$WANT_DEBUG" = "0"; then
    CFLAGS="-DNDEBUG -g $CFLAGS"
    CXXFLAGS="-DNDEBUG -g $CXXFLAGS"
fi

# Override/fixup the version numbers set by AC_INIT, since on
# developer builds, there's no good way to know what the version is
# before running configure :(.  We only use the base version number
# (ie, no svn r numbers) for the version set in AC_INIT.  This will
# always match reality because we add the VERSION file (the only way
# to change the major.minor.release{greek}) into the configure
# dependencies.

PACKAGE_VERSION="$OPENRCM_VERSION"
PACKAGE_STRING="${PACKAGE_NAME} ${PACKAGE_VERSION}"
VERSION="${PACKAGE_VERSION}"

# For standalone configurations, we also include a .so version number.

. $srcdir/VERSION
AC_SUBST([libopenrcm_so_version])

# Setup the header file

AH_TOP([/* -*- c -*-
 *
 * Copyright © 2009-2010 Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 *
 * This is the "private" config header file for ORCM.  This file
 * should only be included by .c files in the ORCM tree.  It should
 * *not* be included by code outside of the ORCM source tree!
 *
 * This file is automatically generated by configure.  Edits will be lost
 * the next time you run configure!
 */

#ifndef OPENRCM_CONFIG_H
#define OPENRCM_CONFIG_H
])
AH_BOTTOM([
#include "openrcm_config_bottom.h"

#endif /* OPENRCM_CONFIG_H */
])

#
# C compiler.  Use ortecc by default if $CC wasn't otherwise specified
# (and if we're not --enable-dist).
#

CFLAGS_save="$CFLAGS"
AS_IF([test "x$CC" = "x" -a "$ORCM_WANT_DIST" != "yes"], [CC=ortecc])
AC_PROG_CC
CFLAGS="$CFLAGS_save"

CXXFLAGS_save="$CXXFLAGS"
AS_IF([test "x$CXX" = "x" -a "$ORCM_WANT_DIST" != "yes"], [CXX=ortec++])
AC_PROG_CXX
CXXFLAGS="$CXXFLAGS_save"

#
# Add appropriate CPPFLAGS to find various include files
#

AC_MSG_CHECKING([for final CPPFLAGS])
CPPFLAGS='-I$(top_srcdir) -I$(top_srcdir)/src -I$(top_srcdir)/src/include'" $ORTE_CPPFLAGS $CPPFLAGS"
AC_MSG_RESULT([$CPPFLAGS])

AC_MSG_CHECKING([for final LDFLAGS])
LDFLAGS="$ORTE_LDFLAGS $LDFLAGS"
AC_MSG_RESULT([$LDFLAGS])

#
# Need to find the OMPI header files and libraries
#

AS_IF([test "$ORCM_WANT_DIST" != "yes"],
      [AC_CHECK_HEADERS([opal/mca/mca.h], [], 
                        [AC_MSG_ERROR([*** Cannot find opal/mca/mca.h])])
       AC_CHECK_LIB([open-pal], [opal_init_util], [],
                    [AC_MSG_ERROR([*** Cannot find/link to libopen-pal])])
       AC_CHECK_LIB([open-rte], [orte_init], [],
                    [AC_MSG_ERROR([*** Cannot find/link to libopen-rte])],
                    [-lopen-pal])
       # RHC: need to find a better public symbol than orte_rmcast
       AC_CHECK_FUNC([orte_rmcast], [],
                     [AC_MSG_ERROR([*** ORTE does not appear to have been built with multicast support.  Cannot continue.])])
])

# Expand/eval $includedir and $libdir for AC_SUBST'ing.

if test "$prefix" = "NONE"; then
   # If --prefix wasn't specified on the configure line, then $prefix
   # will be NONE.  So just default to the GNU default: /usr/local.
   prefix=/usr/local
fi
if test "$exec_prefix" = "NONE"; then
   exec_prefix=$prefix
fi

foo="expanded_incdir=$includedir"
eval $foo
OPENRCM_WRAPPER_INCLUDEDIR="$expanded_incdir"
AC_SUBST(OPENRCM_WRAPPER_INCLUDEDIR)

foo="expanded_libdir=$libdir"
eval $foo

OPENRCM_WRAPPER_LIBDIR="$expanded_libdir"
AC_SUBST(OPENRCM_WRAPPER_LIBDIR)

foo="expanded_datadir=$datadir"
eval $foo
OPENRCM_DATADIR="$expanded_datadir"
AC_SUBST(OPENRCM_DATADIR)

#
# Setup wrapper compiler flags.  Just add the ORTE -I and -L dirs.
#

OPENRCM_WRAPPER_EXTRA_INCLUDES=
OPENRCM_WRAPPER_EXTRA_CPPFLAGS="$ORTE_CPPFLAGS -I$expanded_incdir/openrcm/include -I$expanded_incdir/openrcm"
OPENRCM_WRAPPER_EXTRA_CFLAGS=
OPENRCM_WRAPPER_EXTRA_CXXFLAGS=
OPENRCM_WRAPPER_EXTRA_LDFLAGS=$ORTE_LDFLAGS
OPENRCM_WRAPPER_EXTRA_LIBS=
AC_SUBST(OPENRCM_WRAPPER_EXTRA_INCLUDES)
AC_SUBST(OPENRCM_WRAPPER_EXTRA_CPPFLAGS)
AC_SUBST(OPENRCM_WRAPPER_EXTRA_CFLAGS)
AC_SUBST(OPENRCM_WRAPPER_EXTRA_CXXFLAGS)
AC_SUBST(OPENRCM_WRAPPER_EXTRA_LDFLAGS)
AC_SUBST(OPENRCM_WRAPPER_EXTRA_LIBS)

#
# Libtool (default to static builds)
#

AC_PROG_LIBTOOL
AM_ENABLE_STATIC
AM_DISABLE_SHARED

#
# Output files
#

# Private / ORCM-only header file (i.e., only included in ORCM .c files)
AC_CONFIG_HEADERS([src/include/openrcm_config_private.h])
# Public header that anyone can include
AC_CONFIG_HEADERS([src/include/openrcm.h])

AC_CONFIG_FILES([Makefile
                 src/Makefile

		 src/mca/Makefile

		 src/mca/cfgi/Makefile
                 src/mca/cfgi/base/Makefile
                 src/mca/cfgi/file/Makefile

		 src/mca/clip/Makefile
                 src/mca/clip/base/Makefile
                 src/mca/clip/default/Makefile

		 src/mca/fddp/Makefile
                 src/mca/fddp/base/Makefile
                 src/mca/fddp/trend/Makefile

		 src/mca/leader/Makefile
                 src/mca/leader/base/Makefile
                 src/mca/leader/fifl/Makefile

		 src/mca/pnp/Makefile
                 src/mca/pnp/base/Makefile
                 src/mca/pnp/default/Makefile

		 src/mca/sensor/Makefile
                 src/mca/sensor/base/Makefile
                 src/mca/sensor/pru/Makefile

                 src/tools/Makefile
		 src/tools/orcm/Makefile
		 src/tools/orcm-start/Makefile
		 src/tools/orcm-stop/Makefile
		 src/tools/orcm-ps/Makefile
		 src/tools/wrappers/Makefile
		 src/tools/wrappers/orcmcc-wrapper-data.txt
		 src/tools/wrappers/orcm_wrapper_script
])

#
# Party on
#

AC_OUTPUT

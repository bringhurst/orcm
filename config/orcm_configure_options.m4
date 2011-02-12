dnl -*- shell-script -*-
dnl
dnl Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
dnl
dnl $COPYRIGHT$
dnl 
dnl Additional copyrights may follow
dnl 
dnl $HEADER$
dnl


AC_DEFUN([ORCM_CONFIGURE_OPTIONS],[
orcm_show_subtitle "ORCM Configuration options"

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

AC_MSG_CHECKING([for ORTE wrapper data files])
AC_ARG_WITH([orte-wrapper-dir],
    [AC_HELP_STRING([--with-orte-wrapper-dir],
                    [Directory where ORTE wrapper data files are located])])
happy=0
ORTE_WRAPPER_DATA_DIR="not found"
AS_IF([test "$with_orte_wrapper_dir" != ""],
      [AS_IF([test -d "$with_orte_wrapper_dir" -a -f "$with_orte_wrapper_dir/ortecc-wrapper-data.txt"],
             [happy=1
              ORTE_WRAPPER_DATA_DIR=$with_orte_wrapper_dir])],
      [AS_IF([test -d "$with_orte/share"],
             [files=`find "$with_orte/share" | grep ortecc-wrapper-data.txt`
              num_files=`echo $files | wc -w`
              AS_IF([test $num_files = 1],
                    [ORTE_WRAPPER_DATA_DIR=`echo $files | sed -e s@/ortecc-wrapper-data.txt@@`
	             happy=1
                     AC_SUBST(ORTE_WRAPPER_DATA_DIR)])
             ])
      ])
AC_MSG_RESULT([$ORTE_WRAPPER_DATA_DIR])
AS_IF([test "$happy" = 0],
      [AC_MSG_WARN([Unable to find ORTE wrapper data files.])
       AC_MSG_WARN([Please use the --with-orte-wrapper-dir option to specify])
       AC_MSG_WARN([their location.])
       AC_MSG_ERROR([Cannot continue.])])

AC_MSG_CHECKING([for ORTE plugins])
AC_ARG_WITH([orte-plugin-dir],
    [AC_HELP_STRING([--with-orte-plugin-dir],
                    [Directory where ORTE plugin files are located])])
happy=0
ORTE_PLUGIN_DIR="not found"
AS_IF([test "$with_orte_plugin_dir" != ""],
      [AS_IF([test -d "$with_orte_plugin_dir"],
             [happy=1
              ORTE_PLUGIN_DIR=$with_orte_plugin_dir])],
      [AS_IF([test -d "$with_orte/lib/openmpi-rte"],
             [happy=1
	      ORTE_PLUGIN_DIR="$with_orte/lib/openmpi-rte"])
       AS_IF([test "$happy" = "0" -a -d "$with_orte/lib64/openmpi-rte"],
             [happy=1
	      ORTE_PLUGIN_DIR="$with_orte/lib64/openmpi-rte"])
       AS_IF([test "$happy" = "0" -a -d "$with_orte/lib/openmpi"],
             [happy=1
	      ORTE_PLUGIN_DIR="$with_orte/lib/openmpi"])
       AS_IF([test "$happy" = "0" -a -d "$with_orte/lib64/openmpi"],
             [happy=1
	      ORTE_PLUGIN_DIR="$with_orte/lib64/openmpi"])
      ])
AC_MSG_RESULT([$ORTE_PLUGIN_DIR])
AC_SUBST(ORTE_PLUGIN_DIR)
AS_IF([test "$happy" = 0],
      [AC_MSG_WARN([Unable to find ORTE plugin directory.])
       AC_MSG_WARN([Please use the --with-orte-plugin-dir option to specify])
       AC_MSG_WARN([their location.])
       AC_MSG_ERROR([Cannot continue.])])

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

AC_MSG_CHECKING([if want qlib support])
AC_ARG_WITH([qlib],
            [AC_HELP_STRING([--with-qlib],
            [Build qlib integration (default: no)])])
if test "$with_qlib" = "yes"; then
    AC_MSG_RESULT([yes])
else
    AC_MSG_RESULT([no])
fi
AM_CONDITIONAL(ORCM_WANT_QLIB_LIBADDS, test "$with_qlib" = "yes")

])dnl

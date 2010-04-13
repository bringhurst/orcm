rdnl -*- shell-script -*-
dnl
dnl Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
dnl                         University Research and Technology
dnl                         Corporation.  All rights reserved.
dnl Copyright (c) 2004-2005 The University of Tennessee and The University
dnl                         of Tennessee Research Foundation.  All rights
dnl                         reserved.
dnl Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
dnl                         University of Stuttgart.  All rights reserved.
dnl Copyright (c) 2004-2005 The Regents of the University of California.
dnl                         All rights reserved.
dnl Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
dnl Copyright (c) 2009      Oak Ridge National Labs.  All rights reserved.
dnl Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
dnl
dnl $COPYRIGHT$
dnl 
dnl Additional copyrights may follow
dnl 
dnl $HEADER$
dnl
dnl Portions of this file derived from GASNet v1.12 (see "GASNet"
dnl comments, below)
dnl Copyright 2004,  Dan Bonachea <bonachea@cs.berkeley.edu>
dnl
dnl IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR
dnl DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES ARISING OUT
dnl OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF
dnl CALIFORNIA HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
dnl 
dnl THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES,
dnl INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
dnl AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
dnl ON AN "AS IS" BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
dnl PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
dnl

AC_DEFUN([ORCM_CONFIGURE_SETUP],[

# Some helper script functions.  Unfortunately, we cannot use $1 kinds
# of arugments here because of the m4 substitution.  So we have to set
# special variable names before invoking the function.  :-\

orcm_show_title() {
  cat <<EOF

============================================================================
== ${1}
============================================================================
EOF
}


orcm_show_subtitle() {
  cat <<EOF

*** ${1}
EOF
}


orcm_show_subsubtitle() {
  cat <<EOF

+++ ${1}
EOF
}

orcm_show_subsubsubtitle() {
  cat <<EOF

--- ${1}
EOF
}

#
# Save some stats about this build
#

ORCM_CONFIGURE_USER="`whoami`"
ORCM_CONFIGURE_HOST="`hostname | head -n 1`"
ORCM_CONFIGURE_DATE="`date`"

#
# Save these details so that they can be used in orcm_info later
#
AC_SUBST(ORCM_CONFIGURE_USER)
AC_SUBST(ORCM_CONFIGURE_HOST)
AC_SUBST(ORCM_CONFIGURE_DATE)])dnl

dnl #######################################################################
dnl #######################################################################
dnl #######################################################################

AC_DEFUN([ORCM_BASIC_SETUP],[
#
# Save some stats about this build
#

ORCM_CONFIGURE_USER="`whoami`"
ORCM_CONFIGURE_HOST="`hostname | head -n 1`"
ORCM_CONFIGURE_DATE="`date`"

#
# Make automake clean emacs ~ files for "make clean"
#

CLEANFILES="*~ .\#*"
AC_SUBST(CLEANFILES)

#
# This is useful later (orcm_info, and therefore mpiexec)
#

AC_CANONICAL_HOST
AC_DEFINE_UNQUOTED(OPAL_ARCH, "$host", [ORCM architecture string])

#
# See if we can find an old installation of ORCM to overwrite
#

# Stupid autoconf 2.54 has a bug in AC_PREFIX_PROGRAM -- if orcm_clean
# is not found in the path and the user did not specify --prefix,
# we'll get a $prefix of "."

orcm_prefix_save="$prefix"
AC_PREFIX_PROGRAM(orcm_clean)
if test "$prefix" = "."; then
    prefix="$orcm_prefix_save"
fi
unset orcm_prefix_save

#
# Basic sanity checking; we can't install to a relative path
#

case "$prefix" in
  /*/bin)
    prefix="`dirname $prefix`"
    echo installing to directory \"$prefix\" 
    ;;
  /*) 
    echo installing to directory \"$prefix\" 
    ;;
  NONE)
    echo installing to directory \"$ac_default_prefix\" 
    ;;
  @<:@a-zA-Z@:>@:*)
    echo installing to directory \"$prefix\" 
    ;;
  *) 
    AC_MSG_ERROR(prefix "$prefix" must be an absolute directory path) 
    ;;
esac

# Allow the --enable-dist flag to be passed in

AC_ARG_ENABLE(dist, 
    AC_HELP_STRING([--enable-dist],
		   [guarantee that that the "dist" make target will be functional, although may not guarantee that any other make target will be functional.]),
    ORCM_WANT_DIST=yes, ORCM_WANT_DIST=no)

if test "$ORCM_WANT_DIST" = "yes"; then
    AC_MSG_WARN([Configuring in 'make dist' mode])
    AC_MSG_WARN([Most make targets may be non-functional!])
fi

# BEGIN: Derived from GASNet

# Suggestion from Paul Hargrove to disable --program-prefix and
# friends.  Heavily influenced by GASNet 1.12 acinclude.m4
# functionality to do the same thing (copyright listed at top of this
# file).

# echo program_prefix=$program_prefix  program_suffix=$program_suffix program_transform_name=$program_transform_name
# undo prefix autoconf automatically adds during cross-corcmlation
if test "$cross_corcmling" = yes && test "$program_prefix" = "${target_alias}-" ; then
    program_prefix=NONE
fi
# normalize empty prefix/suffix
if test -z "$program_prefix" ; then
    program_prefix=NONE
fi
if test -z "$program_suffix" ; then
    program_suffix=NONE
fi
# undo transforms caused by empty prefix/suffix
if test "$program_transform_name" = 's,^,,' || \
   test "$program_transform_name" = 's,$$,,' || \
   test "$program_transform_name" = 's,$$,,;s,^,,' ; then
    program_transform_name="s,x,x,"
fi
if test "$program_prefix$program_suffix$program_transform_name" != "NONENONEs,x,x," ; then
    AC_MSG_WARN([*** This configure script does not support --program-prefix, --program-suffix or --program-transform-name. Users are recommended to instead use --prefix with a unique directory and make symbolic links as desired for renaming.])
    AC_MSG_ERROR([*** Cannot continue])
fi

# END: Derived from GASNet
])dnl

dnl #######################################################################
dnl #######################################################################
dnl #######################################################################

AC_DEFUN([ORCM_LOG_MSG],[
# 1 is the message
# 2 is whether to put a prefix or not
if test -n "$2"; then
    echo "configure:__oline__: $1" >&5
else
    echo $1 >&5
fi])dnl

dnl #######################################################################
dnl #######################################################################
dnl #######################################################################

AC_DEFUN([ORCM_LOG_FILE],[
# 1 is the filename
if test -n "$1" -a -f "$1"; then
    cat $1 >&5
fi])dnl

dnl #######################################################################
dnl #######################################################################
dnl #######################################################################

AC_DEFUN([ORCM_LOG_COMMAND],[
# 1 is the command
# 2 is actions to do if success
# 3 is actions to do if fail
echo "configure:__oline__: $1" >&5
$1 1>&5 2>&1
orcm_status=$?
ORCM_LOG_MSG([\$? = $orcm_status], 1)
if test "$orcm_status" = "0"; then
    unset orcm_status
    $2
else
    unset orcm_status
    $3
fi])dnl

dnl #######################################################################
dnl #######################################################################
dnl #######################################################################

AC_DEFUN([ORCM_UNIQ],[
# 1 is the variable name to be uniq-ized
orcm_name=$1

# Go through each item in the variable and only keep the unique ones

orcm_count=0
for val in ${$1}; do
    orcm_done=0
    orcm_i=1
    orcm_found=0

    # Loop over every token we've seen so far

    orcm_done="`expr $orcm_i \> $orcm_count`"
    while test "$orcm_found" = "0" -a "$orcm_done" = "0"; do

	# Have we seen this token already?  Prefix the comparison with
	# "x" so that "-Lfoo" values won't be cause an error.

	orcm_eval="expr x$val = x\$orcm_array_$orcm_i"
	orcm_found=`eval $orcm_eval`

	# Check the ending condition

	orcm_done="`expr $orcm_i \>= $orcm_count`"

	# Increment the counter

	orcm_i="`expr $orcm_i + 1`"
    done

    # If we didn't find the token, add it to the "array"

    if test "$orcm_found" = "0"; then
	orcm_eval="orcm_array_$orcm_i=$val"
	eval $orcm_eval
	orcm_count="`expr $orcm_count + 1`"
    else
	orcm_i="`expr $orcm_i - 1`"
    fi
done

# Take all the items in the "array" and assemble them back into a
# single variable

orcm_i=1
orcm_done="`expr $orcm_i \> $orcm_count`"
orcm_newval=
while test "$orcm_done" = "0"; do
    orcm_eval="orcm_newval=\"$orcm_newval \$orcm_array_$orcm_i\""
    eval $orcm_eval

    orcm_eval="unset orcm_array_$orcm_i"
    eval $orcm_eval

    orcm_done="`expr $orcm_i \>= $orcm_count`"
    orcm_i="`expr $orcm_i + 1`"
done

# Done; do the assignment

orcm_newval="`echo $orcm_newval`"
orcm_eval="$orcm_name=\"$orcm_newval\""
eval $orcm_eval

# Clean up

unset orcm_name orcm_i orcm_done orcm_newval orcm_eval orcm_count])dnl

dnl #######################################################################
dnl #######################################################################
dnl #######################################################################

# Macro that serves as an alternative to using `which <prog>`. It is
# preferable to simply using `which <prog>` because backticks (`) (aka
# backquotes) invoke a sub-shell which may source a "noisy"
# ~/.whatever file (and we do not want the error messages to be part
# of the assignment in foo=`which <prog>`). This macro ensures that we
# get a sane executable value.
AC_DEFUN([ORCM_WHICH],[
# 1 is the variable name to do "which" on
# 2 is the variable name to assign the return value to

ORCM_VAR_SCOPE_PUSH([orcm_prog orcm_file orcm_dir orcm_sentinel])

orcm_prog=$1

IFS_SAVE=$IFS
IFS="$PATH_SEPARATOR"
for orcm_dir in $PATH; do
    if test -x "$orcm_dir/$orcm_prog"; then
        $2="$orcm_dir/$orcm_prog"
        break
    fi
done
IFS=$IFS_SAVE

ORCM_VAR_SCOPE_POP
])dnl

dnl #######################################################################
dnl #######################################################################
dnl #######################################################################

# Declare some variables; use ORCM_VAR_SCOPE_END to ensure that they
# are cleaned up / undefined.
AC_DEFUN([ORCM_VAR_SCOPE_PUSH],[

    # Is the private index set?  If not, set it.
    if test "x$orcm_scope_index" = "x"; then
        orcm_scope_index=1
    fi

    # First, check to see if any of these variables are already set.
    # This is a simple sanity check to ensure we're not already
    # overwriting pre-existing variables (that have a non-empty
    # value).  It's not a perfect check, but at least it's something.
    for orcm_var in $1; do
        orcm_str="orcm_str=\"\$$orcm_var\""
        eval $orcm_str

        if test "x$orcm_str" != "x"; then
            AC_MSG_WARN([Found configure shell variable clash!])
            AC_MSG_WARN([[ORCM_VAR_SCOPE_PUSH] called on "$orcm_var",])
            AC_MSG_WARN([but it is already defined with value "$orcm_str"])
            AC_MSG_WARN([This usually indicates an error in configure.])
            AC_MSG_ERROR([Cannot continue])
        fi
    done

    # Ok, we passed the simple sanity check.  Save all these names so
    # that we can unset them at the end of the scope.
    orcm_str="orcm_scope_$orcm_scope_index=\"$1\""
    eval $orcm_str
    unset orcm_str

    env | grep orcm_scope
    orcm_scope_index=`expr $orcm_scope_index + 1`
])dnl

# Unset a bunch of variables that were previously set
AC_DEFUN([ORCM_VAR_SCOPE_POP],[
    # Unwind the index
    orcm_scope_index=`expr $orcm_scope_index - 1`
    orcm_scope_test=`expr $orcm_scope_index \> 0`
    if test "$orcm_scope_test" = "0"; then
        AC_MSG_WARN([[ORCM_VAR_SCOPE_POP] popped too many ORCM configure scopes.])
        AC_MSG_WARN([This usually indicates an error in configure.])
        AC_MSG_ERROR([Cannot continue])
    fi

    # Get the variable names from that index
    orcm_str="orcm_str=\"\$orcm_scope_$orcm_scope_index\""
    eval $orcm_str

    # Iterate over all the variables and unset them all
    for orcm_var in $orcm_str; do
        unset $orcm_var
    done
])dnl


dnl #######################################################################
dnl #######################################################################
dnl #######################################################################

#
# OPAL_WITH_OPTION_MIN_MAX_VALUE(NAME,DEFAULT_VALUE,LOWER_BOUND,UPPER_BOUND)
# Defines a variable OPAL_MAX_xxx, with "xxx" being specified as parameter $1 as "variable_name".
# If not set at configure-time using --with-max-xxx, the default-value ($2) is assumed.
# If set, value is checked against lower (value >= $3) and upper bound (value <= $4)
#
AC_DEFUN([OPAL_WITH_OPTION_MIN_MAX_VALUE], [
    max_value=[$2]
    AC_MSG_CHECKING([maximum length of ]m4_translit($1, [_], [ ]))
    AC_ARG_WITH([max-]m4_translit($1, [_], [-]),
        AC_HELP_STRING([--with-max-]m4_translit($1, [_], [-])[=VALUE],
                       [maximum length of ]m4_translit($1, [_], [ ])[s.  VALUE argument has to be specified (default: [$2]).]))
    if test ! -z "$with_max_[$1]" -a "$with_max_[$1]" != "no" ; then
        # Ensure it's a number (hopefully an integer!), and >0
        expr $with_max_[$1] + 1 > /dev/null 2> /dev/null
        AS_IF([test "$?" != "0"], [happy=0],
              [AS_IF([test $with_max_[$1] -ge $3 -a $with_max_[$1] -le $4],
                     [happy=1], [happy=0])])

        # If badness in the above tests, bail
        AS_IF([test "$happy" = "0"],
              [AC_MSG_RESULT([bad value ($with_max_[$1])])
               AC_MSG_WARN([--with-max-]m4_translit($1, [_], [-])[s value must be >= $3 and <= $4])
               AC_MSG_ERROR([Cannot continue])])
        max_value=$with_max_[$1]
    fi
    AC_MSG_RESULT([$max_value])
    AC_DEFINE_UNQUOTED([OPAL_MAX_]m4_toupper($1), $max_value,
                       [Maximum length of ]m4_translit($1, [_], [ ])[s (default is $2)])
    [OPAL_MAX_]m4_toupper($1)=$max_value
    AC_SUBST([OPAL_MAX_]m4_toupper($1))
])dnl

dnl -*- shell-script -*-
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
dnl Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
dnl $COPYRIGHT$
dnl 
dnl Additional copyrights may follow
dnl 
dnl $HEADER$
dnl

# ORCM_EVAL_ARG(arg)
# ------------------
# evaluates and returns argument
AC_DEFUN([ORCM_EVAL_ARG], [$1])

######################################################################
#
# ORCM_MCA
#
# configure the MCA (modular component architecture).  Works hand in hand
# with Open MPI's autogen.pl, requiring it's specially formatted lists
# of frameworks, components, etc.
#
# USAGE:
#   ORCM_MCA()
#
######################################################################
AC_DEFUN([ORCM_MCA],[
    dnl for ORCM_CONFIGURE_USER env variable
    AC_REQUIRE([ORCM_CONFIGURE_SETUP])

    # Find which components should be built as run-time loadable components
    # Acceptable combinations:
    #
    # [default -- no option given]
    # --enable-mca-dso
    # --enable-mca-dso=[.+,]*COMPONENT_TYPE[.+,]*
    # --enable-mca-dso=[.+,]*COMPONENT_TYPE-COMPONENT_NAME[.+,]*
    # --disable-mca-dso
    #
    AC_ARG_ENABLE([mca-no-build],
        [AC_HELP_STRING([--enable-mca-no-build=LIST],
                        [Comma-separated list of <type>-<component> pairs 
                         that will not be built.  Example: "--enable-mca-no-build=maffinity,btl-portals" will disable building all maffinity components and the "portals" btl components.])])

    AC_MSG_CHECKING([which components should be disabled])
    if test "$enable_mca_no_build" = "yes"; then
        AC_MSG_RESULT([yes])
        AC_MSG_ERROR([*** The enable-mca-no-build flag requires an explicit list
*** of type-component pairs.  For example, --enable-mca-no-build=pml-ob1])
    else
        ifs_save="$IFS"
        IFS="${IFS}$PATH_SEPARATOR,"
        msg=
        for item in $enable_mca_no_build; do
            type="`echo $item | cut -s -f1 -d-`"
            comp="`echo $item | cut -s -f2- -d-`"
            if test -z $type ; then
                type=$item
            fi
            if test -z $comp ; then
                str="`echo DISABLE_${type}=1 | sed s/-/_/g`"
                eval $str
                msg="$item $msg"
            else
                str="`echo DISABLE_${type}_${comp}=1 | sed s/-/_/g`"
                eval $str
                msg="$item $msg"
            fi
        done
        IFS="$ifs_save"
    fi
    AC_MSG_RESULT([$msg])
    unset msg

    AC_MSG_CHECKING([for projects containing MCA frameworks])
    AC_MSG_RESULT([mca_project_list])

    # if there isn't a project list, abort
    m4_ifdef([mca_project_list], [],
             [m4_fatal([Could not find project list - did autogen.pl complete successfully?])])

    # now configre all the projects, frameworks, and components.  Most
    # of the hard stuff is in here
    MCA_PROJECT_SUBDIRS=
    m4_map_args_pair([MCA_CONFIGURE_PROJECT], [], mca_project_list)
    AC_SUBST(MCA_PROJECT_SUBDIRS)
])


######################################################################
#
# MCA_CONFIGURE_PROJECT
#
# Configure all frameworks inside the given project name.  Assumes that
# the frameworks are located in [project_root]/mca/[frameworks] and that
# there is an m4_defined list named mca_[project]_framework_list with
# the list of frameworks.
#
# USAGE:
#   MCA_CONFIGURE_PROJECT(project_name, project_root)
#
######################################################################
AC_DEFUN([MCA_CONFIGURE_PROJECT],[
    # can't use a variable rename here because these need to be evaled
    # at auto* time.
    m4_define([mcp_name], $1)
    m4_define([mcp_root], $2)

    orcm_show_subtitle "Configuring project mcp_name MCA (dir: mcp_root)"

    MCA_PROJECT_SUBDIRS="$MCA_PROJECT_SUBDIRS mcp_root"

    AC_MSG_CHECKING([for frameworks for mcp_name])
    AC_MSG_RESULT([mca_]mcp_name[_framework_list])

    # iterate through the list of frameworks.  There is something
    # funky with m4 foreach if the list is defined, but empty.  It
    # will call the 3rd argument once with an empty value for the
    # first argument.  Protect against calling MCA_CONFIGURE_FRAMEWORK
    # with an empty second argument.  Grrr....
    # if there isn't a project list, abort
    #
    # Also setup two variables for Makefiles:
    #  MCA_project_FRAMEWORKS     - list of frameworks in that project
    #  MCA_project_FRAMEWORK_LIBS - list of libraries (or variables pointing
    #                               to more libraries) that must be included
    #                               in the project's main library
    m4_ifdef([mca_]mcp_name[_framework_list], [], 
             [m4_fatal([Could not find project list - did autogen.pl complete successfully?])])

    MCA_[]mcp_name[]_FRAMEWORKS=
    MCA_[]mcp_name[]_FRAMEWORKS_SUBDIRS=
    MCA_[]mcp_name[]_FRAMEWORK_COMPONENT_ALL_SUBDIRS=
    MCA_[]mcp_name[]_FRAMEWORK_COMPONENT_DSO_SUBDIRS=
    MCA_[]mcp_name[]_FRAMEWORK_LIBS=
    
    m4_foreach(mca_framework, [mca_]mcp_name[_framework_list],
               [m4_ifval(mca_framework, 
                         [# common has to go up front
                          if test "mca_framework" = "common" ; then
                              MCA_]mcp_name[_FRAMEWORKS="mca_framework $MCA_]mcp_name[_FRAMEWORKS"
                              MCA_]mcp_name[_FRAMEWORKS_SUBDIRS="[mca/]mca_framework $MCA_]mcp_name[_FRAMEWORKS_SUBDIRS"
                              MCA_]mcp_name[_FRAMEWORK_COMPONENT_ALL_SUBDIRS="[\$(MCA_]mcp_name[_]mca_framework[_ALL_SUBDIRS)] $MCA_]mcp_name[_FRAMEWORK_COMPONENT_ALL_SUBDIRS"
                              MCA_]mcp_name[_FRAMEWORK_COMPONENT_DSO_SUBDIRS="[\$(MCA_]mcp_name[_]mca_framework[_DSO_SUBDIRS)] $MCA_]mcp_name[_FRAMEWORK_COMPONENT_DSO_SUBDIRS"
                          else
                              MCA_]mcp_name[_FRAMEWORKS="$MCA_]mcp_name[_FRAMEWORKS mca_framework"
                              MCA_]mcp_name[_FRAMEWORKS_SUBDIRS="$MCA_]mcp_name[_FRAMEWORKS_SUBDIRS [mca/]mca_framework"
                              MCA_]mcp_name[_FRAMEWORK_COMPONENT_ALL_SUBDIRS="$MCA_]mcp_name[_FRAMEWORK_COMPONENT_ALL_SUBDIRS [\$(MCA_]mcp_name[_]mca_framework[_ALL_SUBDIRS)]"
                              MCA_]mcp_name[_FRAMEWORK_COMPONENT_DSO_SUBDIRS="$MCA_]mcp_name[_FRAMEWORK_COMPONENT_DSO_SUBDIRS [\$(MCA_]mcp_name[_]mca_framework[_DSO_SUBDIRS)]"
                          fi
                          if test "mca_framework" != "common" ; then
                              MCA_]mcp_name[_FRAMEWORK_LIBS="$MCA_]mcp_name[_FRAMEWORK_LIBS [mca/]mca_framework[/libmca_]mca_framework[.la]"
                          fi
                          m4_ifdef([MCA_]mca_framework[_CONFIG],
                                   [MCA_]mca_framework[_CONFIG](mcp_name, 
                                                                mca_framework),
                                   [MCA_CONFIGURE_FRAMEWORK(mcp_name, 
                                                            mcp_root, 
                                                            mca_framework, 1)])])])

    AC_SUBST(MCA_[]mcp_name[]_FRAMEWORKS)
    AC_SUBST(MCA_[]mcp_name[]_FRAMEWORKS_SUBDIRS)
    AC_SUBST(MCA_[]mcp_name[]_FRAMEWORK_COMPONENT_ALL_SUBDIRS)
    AC_SUBST(MCA_[]mcp_name[]_FRAMEWORK_COMPONENT_DSO_SUBDIRS)
    AC_SUBST(MCA_[]mcp_name[]_FRAMEWORK_LIBS)
])

######################################################################
#
# MCA_CONFIGURE_FRAMEWORK
#
# Configure the given framework and all components inside the
# framework.  Assumes that the framework is located in
# [project_root]/mca/[framework], and that all components are
# available under the framework directory.  Will configure all
# no-configure and builtin components, then search for components with
# configure scripts.  Assumes that no component is marked as builtin
# AND has a configure script.
#
# USAGE:
#   MCA_CONFIGURE_FRAMEWORK(project_name, project_root, framework_name, 
#                           allow_succeed)
#
######################################################################
AC_DEFUN([MCA_CONFIGURE_FRAMEWORK],[
    m4_define([mcf_name], $1)
    m4_define([mcf_root], $2)
    m4_define([mcf_fw], $3)
    m4_define([mcf_allow_succeed], $4)

    orcm_show_subsubtitle "Configuring mcf_name MCA framework mcf_fw (dir: mcf_root)"

    # setup for framework
    all_components=
    dso_components=

    # Ensure that the directory where the #include file is to live
    # exists.  Need to do this for VPATH builds, because the directory
    # may not exist yet.  For the "common" type, it's not really a
    # component, so it doesn't have a base.
    if test "mcf_fw" = "common" ; then
        outdir=mcf_root/mca/common
    else
        outdir=mcf_root/mca/mcf_fw/base
    fi
    AS_MKDIR_P([$outdir])

    # print some nice messages about what we're about to do...
    AC_MSG_CHECKING([for no configure components in framework mcf_fw])
    AC_MSG_RESULT([mca_]mcf_name[_]mcf_fw[_no_config_component_list])
    AC_MSG_CHECKING([for m4 configure components in framework mcf_fw])
    AC_MSG_RESULT([mca_]mcf_name[_]mcf_fw[_m4_config_component_list])

    # configure components that don't have any component-specific
    # configuration.  See comment in CONFIGURE_PROJECT about the
    # m4_ifval in the m4_foreach.  If there isn't a component list,
    # abort with a reasonable message.  If there are components in the
    # list, but we're doing one of the "special" selection logics,
    # abort with a reasonable message.
    m4_ifdef([mca_]mcf_name[_]mcf_fw[_no_config_component_list], [], 
             [m4_fatal([Could not find project list - did autogen.pl complete successfully?])])
    # make sure priority stuff set right
    m4_if(ORCM_EVAL_ARG([MCA_]mca_framework[_CONFIGURE_MODE]), [STOP_AT_FIRST],
          [m4_ifval(mca_]mcf_name[_]mcf_fw[_no_config_component_list,
                   [m4_fatal([Framework mcf_fw using STOP_AT_FIRST but at least one component has no configure.m4])])])
    m4_if(ORCM_EVAL_ARG([MCA_]mca_framework[_CONFIGURE_MODE]), [STOP_AT_FIRST_PRIORITY],
          [m4_ifval(mca_]mcf_name[_]mcf_fw[_no_config_component_list,
                   [m4_fatal([Framework mcf_fw using STOP_AT_FIRST_PRIORITY but at least one component has no configure.m4])])])
    m4_foreach(mca_component, [mca_]mcf_name[_]mcf_fw[_no_config_component_list],
               [m4_ifval(mca_component,
                  [MCA_CONFIGURE_NO_CONFIG_COMPONENT(mcf_name, 
                                                     mcf_root,
                                                     mcf_fw,
                                                     mca_component,
                                                     [all_components],
                                                     [dso_components],
                                                     [mcf_allow_succeed])])])

    # configure components that use built-in configuration scripts see
    # comment in CONFIGURE_PROJECT about the m4_ifval in the
    # m4_foreach.  if there isn't a component list, abort
    m4_ifdef([mca_]mcf_name[_]mcf_fw[_m4_config_component_list], [], 
             [m4_fatal([Could not find project list - did autogen.pl complete successfully?])])
    best_mca_component_priority=0
    components_looking_for_succeed=mcf_allow_succeed
    components_last_result=0
    m4_foreach(mca_component, [mca_]mcf_name[_]mcf_fw[_m4_config_component_list],
               [m4_ifval(mca_component,
                  [m4_if(ORCM_EVAL_ARG([MCA_]mca_framework[_CONFIGURE_MODE]), [STOP_AT_FIRST_PRIORITY],
                         [ # get the component's priority...
                          infile="mcf_root/mca/mcf_fw/mca_component/configure.params"
                          mca_component_priority="`$GREP PARAM_CONFIG_PRIORITY= $infile | cut -d= -f2-`"
                          AS_IF([test -z "$mca_component_priority"], [mca_component_priority=0])
                          AS_IF([test $best_mca_component_priority -gt $mca_component_priority], [components_looking_for_succeed=0])])
                   MCA_CONFIGURE_M4_CONFIG_COMPONENT(mcf_name,
                                                     mcf_root,
                                                     mcf_fw,
                                                     mca_component, 
                                                     [all_components],
                                                     [dso_components],
                                                     [$components_looking_for_succeed],
                                                     [components_last_result=1],
                                                     [components_last_result=0])
                   m4_if(ORCM_EVAL_ARG([MCA_]mca_framework[_CONFIGURE_MODE]), [STOP_AT_FIRST],
                         [AS_IF([test $components_last_result -eq 1], [components_looking_for_succeed=0])])
                   m4_if(ORCM_EVAL_ARG([MCA_]mca_framework[_CONFIGURE_MODE]), [STOP_AT_FIRST_PRIORITY],
                         [AS_IF([test $components_last_result -eq 1], [best_mca_component_priority=$mca_component_priority])])])])

    MCA_[]mcf_name[_]mcf_fw[]_ALL_COMPONENTS="$all_components"
    MCA_[]mcf_name[_]mcf_fw[]_DSO_COMPONENTS="$dso_components"

    AC_SUBST(MCA_[]mcf_name[_]mcf_fw[]_ALL_COMPONENTS)
    AC_SUBST(MCA_[]mcf_name[_]mcf_fw[]_DSO_COMPONENTS)

    ORCM_MCA_MAKE_DIR_LIST(MCA_[]mcf_name[_]mcf_fw[]_ALL_SUBDIRS, mcf_fw, [$all_components])
    ORCM_MCA_MAKE_DIR_LIST(MCA_[]mcf_name[_]mcf_fw[]_DSO_SUBDIRS, mcf_fw, [$dso_components])

    unset all_components dso_components outfile outfile_real
])


######################################################################
#
# MCA_CONFIGURE_NO_CONFIG_COMPONENT
#
# Configure the given framework and all components inside the framework.
# Assumes that the framework is located in [project_name]/mca/[framework],
# and that all components are available under the framework directory.
# Will configure all builtin components, then search for components with
# configure scripts.  Assumes that no component is marked as builtin
# AND has a configure script.
#
# USAGE:
#   MCA_CONFIGURE_PROJECT(project_name, project_root,
#                         framework_name, component_name
#                         all_components_variable, 
#                         dso_components_variable,
#                         allowed_to_succeed)
#
######################################################################
AC_DEFUN([MCA_CONFIGURE_NO_CONFIG_COMPONENT],[
    m4_define([mcncc_name], $1)
    m4_define([mcncc_root], $2)
    m4_define([mcncc_fw], $3)
    m4_define([mcncc_comp], $4)
    m4_define([mcncc_all_comps], $5)
    m4_define([mcncc_dso_comps], $6)
    m4_define([mcncc_allow_succeed], $7)

    orcm_show_subsubsubtitle "mcncc_name MCA component mcncc_fw:mcncc_comp (no configuration)"

    MCA_COMPONENT_BUILD_CHECK(mcncc_root, mcncc_fw, mcncc_comp, 
                              [should_build=mcncc_allow_succeed], [should_build=0])
    MCA_COMPONENT_COMPILE_MODE(mcncc_name, mcncc_fw, mcncc_comp, compile_mode)

    if test "$should_build" = "1" ; then
        MCA_PROCESS_COMPONENT(mcncc_name, mcncc_root, mcncc_fw, mcncc_comp, mcncc_all_comps, mcncc_dso_comps, $compile_mode)
    else
        MCA_PROCESS_DEAD_COMPONENT(mcncc_name, mcncc_fw, mcncc_comp)
        # add component to all component list
        mcncc_all_comps="$[]mcncc_all_comps[] mcncc_comp"
    fi

    # set the AM_CONDITIONAL on how we should build
    if test "$compile_mode" = "dso" ; then
        BUILD_[]mcncc_name[_]mcncc_fw[_]mcncc_comp[]_DSO=1
    else
        BUILD_[]mcncc_name[_]mcncc_fw[_]mcncc_comp[]_DSO=0
    fi
    AM_CONDITIONAL(ORCM_BUILD_[]mcncc_name[_]mcncc_fw[_]mcncc_comp[]_DSO, test "$BUILD_[]mcncc_name[_]mcncc_fw[_]mcncc_comp[]_DSO" = "1")

    unset compile_mode
])


######################################################################
#
# MCA_CONFIGURE_M4_CONFIG_COMPONENT
#
#
# USAGE:
#   MCA_CONFIGURE_PROJECT(project_name, project_root, 
#                         framework_name, component_name
#                         all_components_variable, 
#                         dso_components_variable,
#                         allowed_to_succeed,
#                         [eval if should build], 
#                         [eval if should not build])
#
######################################################################
AC_DEFUN([MCA_CONFIGURE_M4_CONFIG_COMPONENT],[
    m4_define([mcmcc_name], $1)
    m4_define([mcmcc_root], $2)
    m4_define([mcmcc_fw], $3)
    m4_define([mcmcc_comp], $4)
    m4_define([mcmcc_all_comps], $5)
    m4_define([mcmcc_dso_comps], $6)
    m4_define([mcmcc_allow_succeed], $7)
    m4_define([mcmcc_happy], $8)
    m4_define([mcmcc_sad], $9)

    orcm_show_subsubsubtitle "mcmcc_name MCA component mcmcc_fw:mcmcc_comp (m4 configuration macro)"

    MCA_COMPONENT_BUILD_CHECK(mcmcc_root, mcmcc_fw, mcmcc_comp, [should_build=mcmcc_allow_succeed], [should_build=0])
    # Allow the component to override the build mode if it really wants to.
    # It is, of course, free to end up calling MCA_COMPONENT_COMPILE_MODE
    m4_ifdef([MCA_[]mcmcc_fw[_]mcmcc_comp[]_COMPILE_MODE],
             [MCA_[]mcmcc_fw[_]mcmcc_comp[]_COMPILE_MODE(mcmcc_name, mcmcc_fw, mcmcc_comp, compile_mode)],
             [MCA_COMPONENT_COMPILE_MODE(mcmcc_name, mcmcc_fw, mcmcc_comp, compile_mode)])

    # try to configure the component.  pay no attention to
    # --enable-dist, since we'll always have makefiles.
    AS_IF([test "$should_build" = "1"],
          [m4_ifdef([MCA_]mcmcc_name[_]mcmcc_fw[_]mcmcc_comp[_CONFIG],
                    [MCA_]mcmcc_name[_]mcmcc_fw[_]mcmcc_comp[_CONFIG([should_build=1], 
                                         [should_build=0])],
                    # If they forgot to define an 
                    # MCA_<project_<fw>_<comp>_CONFIG 
                    # macro, print a friendly warning and abort.
                    [AC_MSG_WARN([*** The mcmcc_name:mcmcc_fw:mcmcc_comp did not define an])
                     AC_MSG_WARN([*** MCA_[]mcmcc_name[_]mcmcc_fw[_]mcmcc_comp[]_CONFIG macro in the])
                     AC_MSG_WARN([*** mcmcc_root/mca/mcmcc_fw/mcmcc_comp/configure.m4 file])
                     AC_MSG_ERROR([Cannot continue])])
          ])

    AS_IF([test "$should_build" = "1"],
          [MCA_PROCESS_COMPONENT(mcmcc_name, mcmcc_root, mcmcc_fw, mcmcc_comp, mcmcc_all_comps, mcmcc_dso_comps, $compile_mode)],
          [MCA_PROCESS_DEAD_COMPONENT(mcmcc_name, mcmcc_fw, mcmcc_comp)
           # add component to all component list
           mcmcc_all_comps="$[]mcmcc_all_comps mcmcc_comp"])

    m4_ifdef([MCA_[]mcmcc_name[_]mcmcc_fw[_]mcmcc_comp[]_POST_CONFIG],
             [MCA_[]mcmcc_name[_]mcmcc_fw[_]mcmcc_comp[]_POST_CONFIG($should_build)])

    # set the AM_CONDITIONAL on how we should build
    AS_IF([test "$compile_mode" = "dso"], 
          [BUILD_[]mcmcc_name[_]mcmcc_fw[_]mcmcc_comp[]_DSO=1],
          [BUILD_[]mcmcc_name[_]mcmcc_fw[_]mcmcc_comp[]_DSO=0])
    AM_CONDITIONAL(ORCM_BUILD_[]mcmcc_name[_]mcmcc_fw[_]mcmcc_comp[]_DSO, test "$BUILD_[]$1[_]mcmcc_fw[_]mcmcc_comp[]_DSO" = "1")

    AS_IF([test "$should_build" = "1"], mcmcc_happy, mcmcc_sad)

    unset compile_mode
])


######################################################################
#
# MCA_COMPONENT_COMPILE_MODE
#
# set compile_mode_variable to the compile mode for the given component
#
# USAGE:
#   MCA_COMPONENT_COMPILE_MODE(project_name, 
#                              framework_name, component_name
#                              compile_mode_variable)
#
#   NOTE: component_name may not be determined until runtime....
#
######################################################################
AC_DEFUN([MCA_COMPONENT_COMPILE_MODE],[
    m4_define([mccm_name], $1)
    m4_define([mccm_fw], $2)
    m4_define([mccm_comp], $3)
    m4_define([mccm_cmv], $4)

    project=mccm_name
    framework=mccm_fw
    component=mccm_comp

    # Is this component going to built staic or shared?  $component
    # might not be known until configure time, so have to use eval
    # tricks - can't set variable names at autogen time.
    str="SHARED_FRAMEWORK=\$DSO_$framework"
    eval $str
    str="SHARED_COMPONENT=\$DSO_${framework}_$component"
    eval $str

    # Static is not supported right now; so the only option is DSO
    mccm_cmv=dso

    AC_MSG_CHECKING([for $project MCA component $framework:$component compile mode])
    AC_MSG_RESULT([$mccm_cmv])
])


######################################################################
#
# MCA_PROCESS_COMPONENT
#
# does all setup work for given component.  It should be known before
# calling that this component can build properly (and exists)
#
# USAGE:
#   MCA_PROCESS_COMPONENT(project_name, 
#                         project_root,
#                         framework_name, component_name
#                         all_components_variable,
#                         dso_components_variable,
#                         compile_mode_variable
#
#   NOTE: component_name may not be determined until runtime....
#
######################################################################
AC_DEFUN([MCA_PROCESS_COMPONENT],[
    m4_define([mpc_name], $1)
    m4_define([mpc_root], $2)
    m4_define([mpc_fw], $3)
    m4_define([mpc_comp], $4)
    m4_define([mpc_all_comps], $5)
    m4_define([mpc_dso_comps], $6)
    m4_define([mpc_cmv], $7)

    AC_REQUIRE([AC_PROG_GREP])

    project=mpc_name
    framework=mpc_fw
    component=mpc_comp

    # See if it dropped an output file for us to pick up some
    # shell variables in.  
    infile="$srcdir/mpc_root/mca/$framework/$component/post_configure.sh"

    # Add this subdir to the mast list of all MCA component subdirs
    mpc_all_comps="$[]mpc_all_comps $component"

    if test "mpc_cmv" = "dso" ; then
        mpc_dso_comps="$[]mpc_dso_comps $component"
    else
        AC_MSG_WARN([Unknown component build mode])
        AC_MSG_ERROR([Cannot continue])
    fi

    # Output pretty results
    AC_MSG_CHECKING([if $project MCA component $framework:$component can compile])
    AC_MSG_RESULT([yes])
    
    # If there's an output file, add the values to
    # scope_EXTRA_flags.
    if test -f $infile; then

        # First check for the ABORT tag
        line="`$GREP ABORT= $infile | cut -d= -f2-`"
        if test -n "$line" -a "$line" != "no"; then
            AC_MSG_WARN([mpc_name MCA component configure script told me to abort])
            AC_MSG_ERROR([cannot continue])
        fi
    fi
])


######################################################################
#
# MCA_COMPONENT_BUILD_CHECK
#
# checks the standard rules of component building to see if the 
# given component should be built.
#
# USAGE:
#    MCA_COMPONENT_BUILD_CHECK(project_root, framework, component, 
#                              action-if-build, action-if-not-build)
#
######################################################################
AC_DEFUN([MCA_COMPONENT_BUILD_CHECK],[
    AC_REQUIRE([AC_PROG_GREP])

    m4_define([mcbc_root], $1)
    m4_define([mcbc_fw], $2)
    m4_define([mcbc_comp], $3)
    m4_define([mcbc_happy], $4)
    m4_define([mcbc_sad], $5)

    project_root=mcbc_root
    framework=mcbc_fw
    component=mcbc_comp
    component_path="$srcdir/$project_root/mca/$framework/$component"
    want_component=1

    # if we were explicitly disabled, don't build :)
    str="DISABLED_COMPONENT_CHECK=\$DISABLE_${framework}"
    eval $str
    if test "$DISABLED_COMPONENT_CHECK" = "1" ; then
        want_component=0
    fi
    str="DISABLED_COMPONENT_CHECK=\$DISABLE_${framework}_$component"
    eval $str
    if test "$DISABLED_COMPONENT_CHECK" = "1" ; then
        want_component=0
    fi

    AS_IF([test "$want_component" = "1"], [mcbc_happy], [mcbc_sad])
])


######################################################################
#
# MCA_PROCESS_DEAD_COMPONENT
#
# process a component that can not be built.  Do the last minute checks
# to make sure the user isn't doing something stupid.
#
# USAGE:
#   MCA_PROCESS_DEAD_COMPONENT(project_name, 
#                         framework_name, component_name)
#
#   NOTE: component_name may not be determined until runtime....
#
######################################################################
AC_DEFUN([MCA_PROCESS_DEAD_COMPONENT],[
    m4_define([mpdc_name], $1)
    m4_define([mpdc_fw], $2)
    m4_define([mpdc_comp], $3)

    AC_MSG_CHECKING([if mpfc_name MCA component mpdc_fw:mpdc_comp can compile])
    AC_MSG_RESULT([no])

    # If this component was requested as the default for this
    # type, then abort.
    if test "$with_]mpdc_fw[" = "mpdc_comp" ; then
        AC_MSG_WARN([$1 MCA component "mpdc_comp" failed to configure properly])
        AC_MSG_WARN([This component was selected as the default])
        AC_MSG_ERROR([Cannot continue])
        exit 1
    fi
])

# ORCM_MCA_MAKE_DIR_LIST(subst'ed variable, framework, shell list)
# -------------------------------------------------------------------------
AC_DEFUN([ORCM_MCA_MAKE_DIR_LIST],[
    # Making DSO compnent list: $1
    $1=
    for item in $3 ; do
       $1="$$1 mca/$2/$item"
    done
    AC_SUBST($1)
])

#
# Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

sources = \
        leader_lowest.h \
        leader_lowest.c \
        leader_lowest_component.c

# Make the output library in this directory, and name it either
# mca_<project>_<type>_<name>.la (for DSO builds) or
# libmca_<project>_<type>_<name>.la (for static builds).

if ORCM_BUILD_orcm_leader_lowest_DSO
lib =
lib_sources =
component = mca_orcm_leader_lowest.la
component_sources = $(sources)
else
lib = libmca_orcm_leader_lowest.la
lib_sources = $(sources)
component =
ponent_sources =
endif

mcacomponentdir = $(pkglibdir)
mcacomponent_LTLIBRARIES = $(component)
mca_orcm_leader_lowest_la_SOURCES = $(component_sources)
mca_orcm_leader_lowest_la_LDFLAGS = -module -avoid-version

noinst_LTLIBRARIES = $(lib)
libmca_orcm_leader_lowest_la_SOURCES = $(lib_sources)
libmca_orcm_leader_lowest_la_LDFLAGS = -module -avoid-version

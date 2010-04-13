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
        clip_default.h \
        clip_default.c \
        clip_default_component.c

# Make the output library in this directory, and name it either
# mca_<project>_<type>_<name>.la (for DSO builds) or
# libmca_<project>_<type>_<name>.la (for static builds).

if ORCM_BUILD_orcm_clip_default_DSO
lib =
lib_sources =
component = mca_orcm_clip_default.la
component_sources = $(sources)
else
lib = libmca_orcm_clip_default.la
lib_sources = $(sources)
component =
ponent_sources =
endif

mcacomponentdir = $(pkglibdir)
mcacomponent_LTLIBRARIES = $(component)
mca_orcm_clip_default_la_SOURCES = $(component_sources)
mca_orcm_clip_default_la_LDFLAGS = -module -avoid-version

noinst_LTLIBRARIES = $(lib)
libmca_orcm_clip_default_la_SOURCES = $(lib_sources)
libmca_orcm_clip_default_la_LDFLAGS = -module -avoid-version

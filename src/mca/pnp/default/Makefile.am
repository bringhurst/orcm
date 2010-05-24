#
# Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

dist_pkgdata_DATA = help-pnp-default.txt

sources = \
        pnp_default.h \
        pnp_default.c \
        pnp_default_component.c

# Make the output library in this directory, and name it either
# mca_<project>_<type>_<name>.la (for DSO builds) or
# libmca_<project>_<type>_<name>.la (for static builds).

if ORCM_BUILD_orcm_pnp_default_DSO
lib =
lib_sources =
component = mca_orcm_pnp_default.la
component_sources = $(sources)
else
lib = libmca_orcm_pnp_default.la
lib_sources = $(sources)
component =
ponent_sources =
endif

mcacomponentdir = $(pkglibdir)
mcacomponent_LTLIBRARIES = $(component)
mca_orcm_pnp_default_la_SOURCES = $(component_sources)
mca_orcm_pnp_default_la_LDFLAGS = -module -avoid-version

noinst_LTLIBRARIES = $(lib)
libmca_orcm_pnp_default_la_SOURCES = $(lib_sources)
libmca_orcm_pnp_default_la_LDFLAGS = -module -avoid-version

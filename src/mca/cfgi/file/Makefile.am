#
# Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

dist_pkgdata_DATA = help-cfgi-file.txt

AM_LFLAGS = -Porcm_cfgi_file_
LEX_OUTPUT_ROOT = lex.orcm_cfgi_file_

sources = \
        cfgi_file.h \
        cfgi_file_lex.h \
        cfgi_file.c \
        cfgi_file_component.c \
        cfgi_file_lex.l

# Make the output library in this directory, and name it either
# mca_<project>_<type>_<name>.la (for DSO builds) or
# libmca_<project>_<type>_<name>.la (for static builds).

if ORCM_BUILD_orcm_cfgi_file_DSO
lib =
lib_sources =
component = mca_orcm_cfgi_file.la
component_sources = $(sources)
else
lib = libmca_orcm_cfgi_file.la
lib_sources = $(sources)
component =
ponent_sources =
endif

mcacomponentdir = $(pkglibdir)
mcacomponent_LTLIBRARIES = $(component)
mca_orcm_cfgi_file_la_SOURCES = $(component_sources)
mca_orcm_cfgi_file_la_LDFLAGS = -module -avoid-version

noinst_LTLIBRARIES = $(lib)
libmca_orcm_cfgi_file_la_SOURCES = $(lib_sources)
libmca_orcm_cfgi_file_la_LDFLAGS = -module -avoid-version

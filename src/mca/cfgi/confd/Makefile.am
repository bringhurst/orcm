#
# Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

AM_CPPFLAGS = $(orcm_cfgi_confd_CPPFLAGS)

EXTRA_DIST = orcm.yang

sources = \
        cfgi_confd.h \
        cfgi_confd.c \
        cfgi_confd_component.c

if WANT_LOCAL_CONFD
sources += orcm_q_confd.h \
           orcm_q_confd.c
EXTRA_LIBS = 
else
EXTRA_LIBS = -lnconfd_ulib -lqnlib -lsvmdb -lsvm -lclib -lvlibmemoryclient -lvlibapi
endif

# Extra rules to build the orcm-confd.h file

BUILT_SOURCES = orcm-confd.h

orcm.fxs: orcm.yang
	confdc -c $(srcdir)/orcm.yang

orcm-confd.h: orcm.fxs
	confdc --emit-h orcm-confd.h orcm.fxs

orcm.ccl: orcm.cli
	confdc -c $(srcdir)/orcm.cli

# Make the output library in this directory, and name it either
# mca_<project>_<type>_<name>.la (for DSO builds) or
# libmca_<project>_<type>_<name>.la (for static builds).

if ORCM_BUILD_orcm_cfgi_confd_DSO
lib =
lib_sources =
component = mca_orcm_cfgi_confd.la
component_sources = $(sources)
else
lib = libmca_orcm_cfgi_confd.la
lib_sources = $(sources)
component =
ponent_sources =
endif

mcacomponentdir = $(pkglibdir)
mcacomponent_LTLIBRARIES = $(component)
mca_orcm_cfgi_confd_la_SOURCES = $(component_sources)
nodist_mca_orcm_cfgi_confd_la_SOURCES = orcm-confd.h
mca_orcm_cfgi_confd_la_CPPFLAGS = $(orcm_cfgi_confd_CPPFLAGS)
mca_orcm_cfgi_confd_la_LDFLAGS = -module -avoid-version $(orcm_cfgi_confd_LDFLAGS)
mca_orcm_cfgi_confd_la_LIBADD = $(orcm_cfgi_confd_LIBS) $(EXTRA_LIBS)

noinst_LTLIBRARIES = $(lib)
libmca_orcm_cfgi_confd_la_SOURCES = $(lib_sources)
nodist_libmca_orcm_cfgi_confd_la_SOURCES = orcm-confd.h
libmca_orcm_cfgi_confd_la_CPPFLAGS = $(orcm_cfgi_confd_CPPFLAGS)
libmca_orcm_cfgi_confd_la_LDFLAGS = -module -avoid-version $(orcm_cfgi_confd_LDFLAGS)
libmca_orcm_cfgi_confd_la_LIBADD = $(orcm_cfgi_confd_LIBS) $(EXTRA_LIBS)

yangdir = $(prefix)/etc/confd
nodist_yang_HEADERS = orcm.fxs orcm.ccl

# Clean out the generated files

CLEANFILES = orcm.fxs orcm-confd.h orcm.ccl


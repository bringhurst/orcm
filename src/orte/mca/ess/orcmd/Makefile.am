#
# Copyright (c) 2010 	  Cisco Systems, Inc. All rights reserved.
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

sources = \
        ess_orcmd.h \
        ess_orcmd_component.c \
        ess_orcmd.c

# Building outside of the ORTE tree, we can *ONLY* build components as
# DSOs (because libopen-rte.* has already been created/finalized).

if ORCM_WANT_QLIB_LIBADDS
EXTRA_LIBS = -lqlib -lsvm -lsvmdb -lqnvram -lclib
else
EXTRA_LIBS =
endif

mcacomponentdir = $(pkglibdir)
mcacomponent_LTLIBRARIES = mca_ess_orcmd.la
mca_ess_orcmd_la_SOURCES = $(sources)
mca_ess_orcmd_la_LDFLAGS = -module -avoid-version
mca_ess_orcmd_la_LIBADD = $(EXTRA_LIBS)

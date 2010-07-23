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
        ess_orcmapp.h \
        ess_orcmapp_component.c \
        ess_orcmapp.c

# Building outside of the ORTE tree, we can *ONLY* build components as
# DSOs (because libopen-rte.* has already been created/finalized).

mcacomponentdir = $(pkglibdir)
mcacomponent_LTLIBRARIES = mca_ess_orcmapp.la
mca_ess_orcmapp_la_SOURCES = $(sources)
mca_ess_orcmapp_la_LDFLAGS = -module -avoid-version

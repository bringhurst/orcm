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
        errmgr_orcmapp.h \
        errmgr_orcmapp_component.c \
        errmgr_orcmapp.c

# Building outside of the ORTE tree, we can *ONLY* build components as
# DSOs (because libopen-rte.* has already been created/finalized).

mcacomponentdir = $(pkglibdir)
mcacomponent_LTLIBRARIES = mca_errmgr_orcmapp.la
mca_errmgr_orcmapp_la_SOURCES = $(sources)
mca_errmgr_orcmapp_la_LDFLAGS = -module -avoid-version

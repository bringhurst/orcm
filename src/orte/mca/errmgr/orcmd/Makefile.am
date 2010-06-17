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
        errmgr_orcmd.h \
        errmgr_orcmd_component.c \
        errmgr_orcmd_module.c

# Building outside of the ORTE tree, we can *ONLY* build components as
# DSOs (because libopen-rte.* has already been created/finalized).

mcacomponentdir = $(pkglibdir)
mcacomponent_LTLIBRARIES = mca_errmgr_orcmd.la
mca_errmgr_orcmd_la_SOURCES = $(sources)
mca_errmgr_orcmd_la_LDFLAGS = -module -avoid-version

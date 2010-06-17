#
# Copyright (c) 2010 	  Cisco Systems, Inc. All rights reserved.
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

dist_pkgdata_DATA = help-orte-errmgr-orcm.txt

sources = \
        errmgr_orcm.h \
        errmgr_orcm_component.c \
        errmgr_orcm_module.c

# Building outside of the ORTE tree, we can *ONLY* build components as
# DSOs (because libopen-rte.* has already been created/finalized).

mcacomponentdir = $(pkglibdir)
mcacomponent_LTLIBRARIES = mca_errmgr_orcm.la
mca_errmgr_orcm_la_SOURCES = $(sources)
mca_errmgr_orcm_la_LDFLAGS = -module -avoid-version

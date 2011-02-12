#
# Copyright (c) 2010 	  Cisco Systems, Inc. All rights reserved.
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

dist_pkgdata_DATA = help-orte-errmgr-sched.txt

sources = \
        errmgr_sched.h \
        errmgr_sched_component.c \
        errmgr_sched.c

# Building outside of the ORTE tree, we can *ONLY* build components as
# DSOs (because libopen-rte.* has already been created/finalized).

mcacomponentdir = $(pkglibdir)
mcacomponent_LTLIBRARIES = mca_errmgr_orcmsched.la
mca_errmgr_orcmsched_la_SOURCES = $(sources)
mca_errmgr_orcmsched_la_LDFLAGS = -module -avoid-version

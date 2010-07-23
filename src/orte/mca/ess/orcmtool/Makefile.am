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
        ess_orcmtool.h \
        ess_orcmtool_component.c \
        ess_orcmtool.c

# Building outside of the ORTE tree, we can *ONLY* build components as
# DSOs (because libopen-rte.* has already been created/finalized).

mcacomponentdir = $(pkglibdir)
mcacomponent_LTLIBRARIES = mca_ess_orcmtool.la
mca_ess_orcmtool_la_SOURCES = $(sources)
mca_ess_orcmtool_la_LDFLAGS = -module -avoid-version

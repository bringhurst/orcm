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
        routed_orcm.h \
        routed_orcm_component.c \
        routed_orcm.c

# Building outside of the ORTE tree, we can *ONLY* build components as
# DSOs (because libopen-rte.* has already been created/finalized).

mcacomponentdir = $(pkglibdir)
mcacomponent_LTLIBRARIES = mca_routed_orcm.la
mca_routed_orcm_la_SOURCES = $(sources)
mca_routed_orcm_la_LDFLAGS = -module -avoid-version

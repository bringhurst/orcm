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
        iof_orcm.h \
        iof_orcm_component.c \
        iof_orcm.c \
        iof_orcm_receive.c

# Building outside of the ORTE tree, we can *ONLY* build components as
# DSOs (because libopen-rte.* has already been created/finalized).

ortemcacomponentdir = $(ORTE_PLUGIN_DIR)
ortemcacomponent_LTLIBRARIES = mca_iof_orcm.la
mca_iof_orcm_la_SOURCES = $(sources)
mca_iof_orcm_la_LDFLAGS = -module -avoid-version

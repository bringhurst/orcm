#
# Copyright (c) 2010 	  Cisco Systems, Inc. All rights reserved.
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

dist_pkgdata_DATA = help-orte-plm-orcm.txt

sources = \
        plm_orcm.h \
        plm_orcm_component.c \
        plm_orcm_module.c

# Building outside of the ORTE tree, we can *ONLY* build components as
# DSOs (because libopen-rte.* has already been created/finalized).

ortemcacomponentdir = $(ORTE_PLUGIN_DIR)
ortemcacomponent_LTLIBRARIES = mca_plm_orcm.la
mca_plm_orcm_la_SOURCES = $(sources)
mca_plm_orcm_la_LDFLAGS = -module -avoid-version

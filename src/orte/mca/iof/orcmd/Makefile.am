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
        iof_orcmd.h \
        iof_orcmd_component.c \
        iof_orcmd.c \
        iof_orcmd_read.c

# Building outside of the ORTE tree, we can *ONLY* build components as
# DSOs (because libopen-rte.* has already been created/finalized).

mcacomponentdir = $(pkglibdir)
mcacomponent_LTLIBRARIES = mca_iof_orcmd.la
mca_iof_orcmd_la_SOURCES = $(sources)
mca_iof_orcmd_la_LDFLAGS = -module -avoid-version

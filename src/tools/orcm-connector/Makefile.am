#
# Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

dist_pkgdata_DATA = help-orcm-connector.txt

bin_PROGRAMS = orcm-connector
orcm_connector_SOURCES = orcm-connector.c
orcm_connector_DEPENDENCIES = $(top_builddir)/src/libopenrcm.la
orcm_connector_LDADD = $(orcm_connector_DEPENDENCIES) -lopen-rte

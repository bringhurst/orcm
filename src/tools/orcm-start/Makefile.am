#
# Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

dist_pkgdata_DATA = help-orcm-start.txt

bin_PROGRAMS = orcm-start
orcm_start_SOURCES = orcm-start.c
orcm_start_DEPENDENCIES = $(top_builddir)/src/libopenrcm.la
orcm_start_LDADD = $(orcm_start_DEPENDENCIES) -lopen-rte

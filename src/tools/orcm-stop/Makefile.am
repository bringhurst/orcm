#
# Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

dist_pkgdata_DATA = help-orcm-stop.txt

bin_PROGRAMS = orcm-stop
orcm_stop_SOURCES = orcm-stop.c
orcm_stop_DEPENDENCIES = $(top_builddir)/src/libopenrcm.la
orcm_stop_LDADD = $(orcm_stop_DEPENDENCIES) -lopen-rte

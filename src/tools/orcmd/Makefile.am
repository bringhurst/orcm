#
# Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

dist_pkgdata_DATA = help-orcmd.txt

bin_PROGRAMS = orcmd
orcmd_SOURCES = orcmd.c

dist_bin_SCRIPTS = orcmd.qn.sh

orcmd_DEPENDENCIES = $(top_builddir)/src/libopenrcm.la
orcmd_LDADD = $(orcmd_DEPENDENCIES) -lopen-rte

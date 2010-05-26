#
# Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

dist_pkgdata_DATA = help-orcm-sched.txt

bin_PROGRAMS = orcm-sched
orcm_sched_SOURCES = orcm-sched.c
orcm_sched_DEPENDENCIES = $(top_builddir)/src/libopenrcm.la
orcm_sched_LDADD = $(orcm_sched_DEPENDENCIES) -lopen-rte

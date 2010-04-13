
# Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

dist_pkgdata_DATA = help-orcm-vm.txt

bin_PROGRAMS = orcm-vm
orcm_vm_SOURCES = orcm-vm.c
orcm_vm_DEPENDENCIES = $(top_builddir)/src/libopenrcm.la
orcm_vm_LDADD = $(orcm_vm_DEPENDENCIES) -lopen-rte

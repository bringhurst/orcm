#
# Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

SUBDIRS = base trend

headers = fddp.h

# install the header files where they can be found
nobase_openrcm_HEADERS = $(headers)
openrcmdir = $(includedir)/openrcm/mca/fddp

noinst_LTLIBRARIES = libmca_fddp.la
libmca_fddp_la_SOURCES = $(headers)
libmca_fddp_la_DEPENDENCIES = \
        base/libmca_fddp_base.la \
        trend/libmca_fddp_trend.la
libmca_fddp_la_LIBADD = $(libmca_fddp_la_DEPENDENCIES)


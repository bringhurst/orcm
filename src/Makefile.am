#
# Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#
# setup collectors for file names

headers =
libopenrcm_la_SOURCES =
dist_pkgdata_DATA =

SUBDIRS = mca . tools

# add in the subdirectory data
include include/Makefile.am
include runtime/Makefile.am
include util/Makefile.am

# install the header files where they can be found
nobase_openrcm_HEADERS = $(headers)
openrcmdir = $(includedir)/openrcm

lib_LTLIBRARIES = libopenrcm.la
# AM 1.11 seems to require an empty SOURCES line :-(
libopenrcm_la_DEPENDENCIES = \
        mca/libopenrcm_mca.la
libopenrcm_la_LIBADD = $(libopenrcm_la_DEPENDENCIES)

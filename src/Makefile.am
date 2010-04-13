#
# Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

headers =
libopenrcm_la_SOURCES =
dist_pkgdata_DATA =

SUBDIRS = \
        orte \
	$(MCA_orcm_FRAMEWORKS_SUBDIRS) \
	$(MCA_orcm_FRAMEWORK_COMPONENT_STATIC_SUBDIRS) \
        . \
	$(MCA_orcm_FRAMEWORK_COMPONENT_DSO_SUBDIRS) \
        tools

DIST_SUBDIRS = \
        orte \
	$(MCA_orcm_FRAMEWORKS_SUBDIRS) \
	$(MCA_orcm_FRAMEWORK_COMPONENT_ALL_SUBDIRS) \
        tools

# Add in the subdirectory data
include include/Makefile.include
include runtime/Makefile.include
include util/Makefile.include

# Install the header files where they can be found
nobase_openrcm_HEADERS = $(headers)
openrcmdir = $(includedir)/openrcm

# Install the main ORCM library
lib_LTLIBRARIES = libopenrcm.la
libopenrcm_la_LIBADD = $(MCA_orcm_FRAMEWORK_LIBS)
libopenrcm_la_DEPENDENCIES = $(libopenrcm_la_LIBADD)
libopenrcm_la_LDFLAGS = -version-info $(libopenrcm_so_version) -g

#
# Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

SUBDIRS = base default

headers = clip.h

# install the header files where they can be found
nobase_openrcm_HEADERS = $(headers)
openrcmdir = $(includedir)/openrcm/mca/clip

noinst_LTLIBRARIES = libmca_clip.la
libmca_clip_la_SOURCES = $(headers)
libmca_clip_la_DEPENDENCIES = \
        base/libmca_clip_base.la \
        default/libmca_clip_default.la
libmca_clip_la_LIBADD = $(libmca_clip_la_DEPENDENCIES)


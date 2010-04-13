#
# Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

# Main library
noinst_LTLIBRARIES = libmca_clip.la
headers = clip.h
sources =

include base/Makefile.include

libmca_clip_la_SOURCES = $(headers) $(sources)

# Install the header files where they can be found
openrcmdir = $(includedir)/openrcm/mca/clip
nobase_openrcm_HEADERS = $(headers)

distclean-local:
	rm -f base/static-components.h

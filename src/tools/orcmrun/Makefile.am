#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2009 High Performance Computing Center Stuttgart, 
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2008-2010 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

# This is not quite in the Automake spirit, but we have to do it.
# Since the totalview portion of the library must be built with -g, we
# must eliminate the CFLAGS that are passed in here by default (which
# may already have debugging and/or optimization flags).  We use
# post-processed forms of the CFLAGS in the library targets down
# below.

CFLAGS = $(CFLAGS_WITHOUT_OPTFLAGS) $(DEBUGGER_CFLAGS)

include $(top_srcdir)/Makefile.man-page-rules

man_pages = orcmrun.1
EXTRA_DIST = $(man_pages:.1=.1in)

bin_PROGRAMS = orcmrun

nodist_man_MANS = $(man_pages)

# Ensure that the man pages are rebuilt if the opal_config.h file
# changes; a "good enough" way to know if configure was run again (and
# therefore the release date or version may have changed)
$(nodist_man_MANS): $(top_builddir)/config/autogen_found_items.m4

dist_pkgdata_DATA = help-orcmrun.txt

orcmrun_SOURCES = \
        main.c \
        orcmrun.c \
        orcmrun.h

orcmrun_DEPENDENCIES = $(top_builddir)/src/libopenrcm.la
orcmrun_LDADD = $(orcmrun_DEPENDENCIES) -lopen-rte

distclean-local:
	rm -f $(man_pages)

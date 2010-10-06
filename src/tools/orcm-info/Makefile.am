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
# Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

AM_CFLAGS = \
            -DORCM_CONFIGURE_USER="\"@ORCM_CONFIGURE_USER@\"" \
            -DORCM_CONFIGURE_HOST="\"@ORCM_CONFIGURE_HOST@\"" \
            -DORCM_CONFIGURE_DATE="\"@ORCM_CONFIGURE_DATE@\"" \
            -DORCM_BUILD_USER="\"$$USER\"" \
            -DORCM_BUILD_HOST="\"`hostname`\"" \
            -DORCM_BUILD_DATE="\"`date`\"" \
            -DORCM_BUILD_CFLAGS="\"@CFLAGS@\"" \
            -DORCM_BUILD_CPPFLAGS="\"@CPPFLAGS@\"" \
            -DORCM_BUILD_LDFLAGS="\"@LDFLAGS@\"" \
            -DORCM_BUILD_LIBS="\"@LIBS@\""
include $(top_srcdir)/Makefile.man-page-rules

man_pages = orcm-info.1
EXTRA_DIST = $(man_pages:.1=.1in)

bin_PROGRAMS = orcm-info

dist_pkgdata_DATA = help-orcm-info.txt

nodist_man_MANS = $(man_pages)

# Ensure that the man pages are rebuilt if the openrcm_config_private.h file
# changes; a "good enough" way to know if configure was run again (and
# therefore the release date or version may have changed)
$(nodist_man_MANS): $(top_builddir)/src/include/openrcm_config_private.h


orcm_info_SOURCES = \
        orcm-info.h \
        orcm-info.c \
        output.c \
        param.c \
        components.c \
        version.c

orcm_info_DEPENDENCIES = $(top_builddir)/src/libopenrcm.la
orcm_info_LDADD = $(orcm_info_DEPENDENCIES) -lopen-rte

distclean-local:
	rm -f $(man_pages)

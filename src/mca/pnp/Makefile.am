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

headers = pnp.h \
          pnp_types.h

# install the header files where they can be found
nobase_openrcm_HEADERS = $(headers)
openrcmdir = $(includedir)/openrcm/mca/pnp

noinst_LTLIBRARIES = libmca_pnp.la
libmca_pnp_la_SOURCES = $(headers)
libmca_pnp_la_DEPENDENCIES = \
        base/libmca_pnp_base.la \
        default/libmca_pnp_default.la
libmca_pnp_la_LIBADD = $(libmca_pnp_la_DEPENDENCIES)


#
# Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

# Subdirectories are traversed *in order*.  I.e., when "make all"
# comes through, AM will invoke "make all" in bar, then "make all" in
# foo, and then "make all" in this Makefile.am.  To be precise, if "."
# is not in SUBDIRS, this Makefile.am is processed after all the dirs
# listed in SUBDIRS).
SUBDIRS = cfgi clip leader pnp

# The "noinst" prefix means that this library will not be installed,
# but it will be built (i.e., its only purpose in life is to be
# entirely slurped into a higher-level library -- in this case, the
# top-level libcrs.la that is built in src/; this is what LT calls a
# "convenience library").  The LTLIBRARIES suffix means to use Libtool
# to build the library.  The use of LTLIBRARIES means that the
# libraries must end in ".la".
noinst_LTLIBRARIES = libopenrcm_mca.la

# In this case, we have no SOURCES for libopenrcm_mca.la -- we just
# want to slurp in the subdirectory convenience libraries.  But it
# seems that AM 1.11 requires an empty SOURCES line :-(
libopenrcm_mca_la_SOURCES =

# It is important to list the convenience libraries as dependencies so
# that if they ever change, libopenrcm_mca.la is re-built (i.e.,
# re-slurps all the convenience libraries).
libopenrcm_mca_la_DEPENDENCIES = \
        cfgi/libmca_cfgi.la \
        clip/libmca_clip.la \
        leader/libmca_leader.la \
        pnp/libmca_pnp.la

# The LIBADD line is where you list the subdirectory convenience
# libraries.  Hence, libopenrcm_mca.la will effectively be the union of
# all these convenience libraries.
libopenrcm_mca_la_LIBADD = $(libopenrcm_mca_la_DEPENDENCIES)

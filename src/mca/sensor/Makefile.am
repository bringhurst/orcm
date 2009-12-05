#
# Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

SUBDIRS = base pru

headers = sensor.h \
          sensor_types.h

# install the header files where they can be found
nobase_openrcm_HEADERS = $(headers)
openrcmdir = $(includedir)/openrcm/mca/sensor

noinst_LTLIBRARIES = libmca_sensor.la
libmca_sensor_la_SOURCES = $(headers)
libmca_sensor_la_DEPENDENCIES = \
        base/libmca_sensor_base.la \
        pru/libmca_sensor_pru.la
libmca_sensor_la_LIBADD = $(libmca_sensor_la_DEPENDENCIES)

#        csm/libmca_sensor_csm.la


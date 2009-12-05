SUBDIRS = base fifl

headers = leader.h

# install the header files where they can be found
nobase_openrcm_HEADERS = $(headers)
openrcmdir = $(includedir)/openrcm/mca/leader

noinst_LTLIBRARIES = libmca_leader.la
libmca_leader_la_SOURCES = $(headers)
libmca_leader_la_DEPENDENCIES = \
        base/libmca_leader_base.la \
        fifl/libmca_leader_fifl.la
libmca_leader_la_LIBADD = $(libmca_leader_la_DEPENDENCIES)


SUBDIRS = base file

headers = cfgi.h

# install the header files where they can be found
nobase_openrcm_HEADERS = $(headers)
openrcmdir = $(includedir)/openrcm/mca/cfgi

noinst_LTLIBRARIES = libmca_cfgi.la
libmca_cfgi_la_SOURCES = $(headers)
libmca_cfgi_la_DEPENDENCIES = \
        base/libmca_cfgi_base.la \
        file/libmca_cfgi_file.la
#        file/libmca_cfgi_tool.la
libmca_cfgi_la_LIBADD = $(libmca_cfgi_la_DEPENDENCIES)


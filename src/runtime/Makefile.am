dist_pkgdata_DATA = help-openrcm-runtime.txt

headers = runtime.h

# install the header files where they can be found
nobase_openrcm_HEADERS = $(headers)
openrcmdir = $(includedir)/openrcm/runtime

noinst_LTLIBRARIES = libruntime.la
libruntime_la_SOURCES = \
        orcm_init.c \
        orcm_finalize.c

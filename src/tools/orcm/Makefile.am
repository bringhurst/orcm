dist_pkgdata_DATA = help-orcm.txt

bin_PROGRAMS = orcm
orcm_SOURCES = orcm.c
orcm_DEPENDENCIES = $(top_builddir)/src/libopenrcm.la
orcm_LDADD = $(orcm_DEPENDENCIES) -lopen-rte

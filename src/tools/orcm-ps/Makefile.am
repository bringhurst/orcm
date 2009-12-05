dist_pkgdata_DATA = help-orcm-ps.txt

bin_PROGRAMS = orcm-ps
orcm_ps_SOURCES = orcm-ps.c
orcm_ps_DEPENDENCIES = $(top_builddir)/src/libopenrcm.la
orcm_ps_LDADD = $(orcm_ps_DEPENDENCIES) -lopen-rte

SUBDIRS = include runtime util mca . tools

lib_LTLIBRARIES = libopenrcm.la
# AM 1.11 seems to require an empty SOURCES line :-(
libopenrcm_la_SOURCES =
libopenrcm_la_DEPENDENCIES = \
        runtime/libruntime.la \
        util/libutil.la \
        mca/libopenrcm_mca.la
libopenrcm_la_LIBADD = $(libopenrcm_la_DEPENDENCIES)

#
# Copyright (c) 2009 Cisco Systems, Inc.  All rights reserved.
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

man_pages = orcmcc.1

if OPAL_WANT_SCRIPT_WRAPPER_COMPILERS

bin_SCRIPTS = openrcm_wrapper_script
CLEANFILES = $(bin_SCRIPTS)

install-exec-hook:
	test -z "$(bindir)" || $(mkdir_p) "$(DESTDIR)$(bindir)"
	(cd $(DESTDIR)$(bindir); rm -f orcmcc; $(LN_S) orcm_wrapper_script orcmcc)
	(cd $(DESTDIR)$(bindir); rm -f orcmc++; $(LN_S) orcm_wrapper_script orcmc++)

uninstall-local:
	rm -f $(DESTDIR)$(bindir)/orcmcc
	rm -f $(DESTDIR)$(bindir)/orcmc++

else # OPAL_WANT_SCRIPT_WRAPPER_COMPILERS

nodist_pkgdata_DATA = \
	orcmcc-wrapper-data.txt

man_MANS = $(man_pages)

install-exec-hook:
	test -z "$(bindir)" || $(mkdir_p) "$(DESTDIR)$(bindir)"
	(cd $(DESTDIR)$(bindir); rm -f orcmcc$(EXEEXT); $(LN_S) @ORTE_INSTALL_PREFIX@/bin/opal_wrapper orcmcc)
	(cd $(DESTDIR)$(bindir); rm -f orcmcc++$(EXEEXT); $(LN_S) @ORTE_INSTALL_PREFIX@/bin/opal_wrapper orcmcc++)
	(rm -f @ORTE_INSTALL_PREFIX@/share/openmpi-rte/orcmcc-wrapper-data.txt; $(LN_S) @OPENRCM_DATADIR@/openrcm/orcmcc-wrapper-data.txt @ORTE_INSTALL_PREFIX@/share/openmpi-rte/orcmcc-wrapper-data.txt)

orcmcc.1: @ORTE_INSTALL_PREFIX@/share/man/man1/ortecc.1
	rm -f orcmcc.1
	sed -e 's/#COMMAND#/orcmcc/g' -e 's/#PROJECT#/OPENRCMLIB/g' -e 's/#PROJECT_SHORT#/OPENRCM/g' -e 's/#LANGUAGE#/C/g' < @ORTE_INSTALL_PREFIX@/share/man/man1/opal_wrapper.1 > orcmcc.1

uninstall-local:
	rm -f $(DESTDIR)$(bindir)/orcmcc$(EXEEXT)
endif

distclean-local:
	rm -f $(man_pages)


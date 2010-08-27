#
# Copyright (c) 2009 Cisco Systems, Inc.  All rights reserved.
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

#man_pages = orcmcc.1

bin_SCRIPTS = orcm_wrapper_script

dist_pkgdata_DATA = \
	orcmcc-wrapper-data.txt \
	orcm_wrapper_script

if OPENRCM_WANT_SCRIPT_WRAPPER_COMPILERS

install-exec-hook:
	test -z "$(bindir)" || $(mkdir_p) "$(DESTDIR)$(bindir)"
	(cd $(DESTDIR)$(bindir); rm -f orcmcc; $(LN_S) orcm_wrapper_script orcmcc)
	(cd $(DESTDIR)$(bindir); rm -f orcmc++; $(LN_S) orcm_wrapper_script orcmc++)

uninstall-local:
	rm -f $(DESTDIR)$(bindir)/orcmcc
	rm -f $(DESTDIR)$(bindir)/orcmc++

else # OPENRCM_WANT_SCRIPT_WRAPPER_COMPILERS

man_MANS = $(man_pages)

install-exec-hook:
	test -z "$(bindir)" || $(mkdir_p) "$(DESTDIR)$(bindir)"
	(cd $(DESTDIR)$(bindir); rm -f orcmcc$(EXEEXT); cp -f @ORTE_INSTALL_PREFIX@/bin/opal_wrapper orcmcc)
	(cd $(DESTDIR)$(bindir); rm -f orcmcc++$(EXEEXT); cp -f @ORTE_INSTALL_PREFIX@/bin/opal_wrapper orcmcc++)

install-data-hook:
	(rm -f @ORTE_WRAPPER_DATA_DIR@/orcmcc-wrapper-data.txt; \
	    cp -f $(DESTDIR)@OPENRCM_DATADIR@/openrcm/orcmcc-wrapper-data.txt @ORTE_WRAPPER_DATA_DIR@/orcmcc-wrapper-data.txt)

#orcmcc.1: @ORTE_INSTALL_PREFIX@/share/man/man1/ortecc.1
#	rm -f orcmcc.1
#	sed -e 's/#COMMAND#/orcmcc/g' -e 's/#PROJECT#/OPENRCMLIB/g' -e 's/#PROJECT_SHORT#/OPENRCM/g' -e 's/#LANGUAGE#/C/g' < @ORTE_INSTALL_PREFIX@/share/man/man1/opal_wrapper.1 > orcmcc.1

uninstall-local:
	rm -f $(DESTDIR)$(bindir)/orcmcc$(EXEEXT)
endif

distclean-local:
#	rm -f $(man_pages)


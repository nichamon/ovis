nobase_include_HEADERS = ovis-test/test.h ovis-lib-config.h
EXTRA_DIST=ovis-lib-configvars.sh.in
CLEANFILES = ovis-lib-configvars.sh
DISTCLEANFILES = $(nobase_include_HEADERS)

do_subst = @LIB_SUBST_RULE@

%.sh: %.sh.in
	$(do_subst) < $< > $@
	chmod 755 $@

SUBDIRS = third
SUBDIRS += json

if ENABLE_OVIS_UTIL
SUBDIRS += ovis_util
endif

if ENABLE_COLL
SUBDIRS += coll
endif

if ENABLE_MMALLOC
SUBDIRS += mmalloc
endif

if ENABLE_OVIS_AUTH
SUBDIRS += ovis_auth
endif

if ENABLE_OVIS_EVENT
SUBDIRS += ovis_event
endif

if ENABLE_ZAP
SUBDIRS += zap
endif

if ENABLE_OVIS_CTRL
SUBDIRS += ovis_ctrl
endif

install-data-hook: ovis-lib-configvars.sh
	$(MKDIR_P) $(DESTDIR)$(libdir)
	$(INSTALL) -m 755 ovis-lib-configvars.sh $(DESTDIR)$(libdir)

uninstall-hook:
	rm $(DESTDIR)$(libdir)/ovis-lib-configvars.sh

include $(top_srcdir)/ldms/rules.mk

AM_LDFLAGS = @OVIS_LIB_ABS@
AM_CPPFLAGS = $(DBGFLAGS) @OVIS_INCLUDE_ABS@

pkglib_LTLIBRARIES = libstore_otel.la

if ENABLE_STORE_OTEL
libstore_otel_la_SOURCES = store_otel.c
libstore_otel_la_LIBADD = \
    $(top_builddir)/ldms/src/core/libldms.la \
    $(top_builddir)/ldms/src/ldmsd/libldmsd_plugattr.la \
    $(top_builddir)/lib/src/coll/libcoll.la \
    $(top_builddir)/lib/src/ovis_util/libovis_util.la \
    -lcurl
endif

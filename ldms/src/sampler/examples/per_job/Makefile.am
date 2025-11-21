include $(top_srcdir)/ldms/rules.mk


pkglib_LTLIBRARIES =
lib_LTLIBRARIES =
check_PROGRAMS =
dist_man7_MANS =

AM_CPPFLAGS = @OVIS_INCLUDE_ABS@
AM_LDFLAGS = @OVIS_LIB_ABS@
COMMON_LIBADD = $(top_builddir)/ldms/src/core/libldms.la \
		$(top_builddir)/lib/src/ovis_util/libovis_util.la \
		$(top_builddir)/lib/src/coll/libcoll.la

libtest_per_job_sampler_la_SOURCES = test_per_job_sampler.c
libtest_per_job_sampler_la_LIBADD = $(COMMON_LIBADD)

pkglib_LTLIBRARIES += libtest_per_job_sampler.la

include $(top_srcdir)/ldms/rules.mk


pkglib_LTLIBRARIES =
lib_LTLIBRARIES =
check_PROGRAMS =
dist_man7_MANS =

AM_CPPFLAGS = @OVIS_INCLUDE_ABS@
AM_LDFLAGS = @OVIS_LIB_ABS@
COMMON_LIBADD = $(top_builddir)/ldms/src/core/libldms.la \
		$(top_builddir)/lib/src/ovis_util/libovis_util.la \
		$(top_builddir)/lib/src/coll/libcoll.la \
		$(top_builddir)/ldms/src/sampler/libper_job_sampler_base.la

libper_job_stat_la_SOURCES = per_job_stat.c
libper_job_stat_la_LIBADD = $(COMMON_LIBADD)

pkglib_LTLIBRARIES += libper_job_stat.la

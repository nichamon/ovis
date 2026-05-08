include $(top_srcdir)/ldms/rules.mk

AM_CPPFLAGS = @OVIS_INCLUDE_ABS@
AM_LDFLAGS = @OVIS_LIB_ABS@

CORE_LIBADD = $(top_builddir)/ldms/src/core/libldms.la \
	      @LDFLAGS_GETTIME@ \
	      $(top_builddir)/lib/src/ovis_util/libovis_util.la

PER_JOB_LIBADD = $(top_builddir)/ldms/src/sampler/libper_job_sampler_base.la \
		 $(CORE_LIBADD)

pkglib_LTLIBRARIES =

if ENABLE_CGROUP_SAMPLER
libcgroup_sampler_la_SOURCES = cgroup_sampler.c \
			       cgroup_util.c \
			       cgroup_util.h
libcgroup_sampler_la_LIBADD = $(PER_JOB_LIBADD)
pkglib_LTLIBRARIES += libcgroup_sampler.la
endif

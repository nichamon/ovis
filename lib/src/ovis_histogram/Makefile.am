lib_LTLIBRARIES =

AM_CFLAGS = -I$(srcdir) -I$(srcdir)/..

libovis_histogramincludedir = $(includedir)/ovis_histogram
libovis_histograminclude_HEADERS = ovis_histogram.h

libovis_histogram_la_SOURCES = ovis_histogram.c ovis_histogram.h
libovis_histogram_la_LIBADD = ../ovis_json/libovis_json.la \
			     -lpthread \
			     -lm
lib_LTLIBRARIES += libovis_histogram.la

AM_CFLAGS = -Wall

AM_CPPFLAGS = \
  -I$(top_builddir)/lib \
  -I$(top_srcdir)/lib

noinst_LIBRARIES = libcommon.a

libcommon_a_SOURCES = \
  bksearch.c \
  bksearch.h \
  byteq.c \
  byteq.h \
  comparison.h \
  error.c \
  error.h \
  hmap.c \
  hmap.h \
  intutil.c \
  intutil.h \
  minmaxonce.h \
  msgq.c \
  msgq.h \
  optparser.c \
  optparser.h \
  ptrv.c \
  ptrv.h \
  quoting.c \
  quoting.h \
  range.c \
  range.h \
  substrcmp.c \
  substrcmp.h \
  strbuf.c \
  strbuf.h \
  strleftcmp.c \
  strleftcmp.h \
  swap.h \
  tempdir.c \
  tempdir.h \
  tempfailure.h \
  tmap.c \
  tmap.h

libcommon_a_LIBADD = \
  ../../lib/libgnu.a

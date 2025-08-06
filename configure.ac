dnl required version of autoconf
AC_PREREQ([2.53])

AC_INIT([rtsp-dash-streamer],[1.0.0])

dnl required version of gstreamer and gst-plugins-base  
GST_REQUIRED=1.0.0

AC_CONFIG_SRCDIR([src/rtsp-dash-streamer.cpp])
AC_CONFIG_HEADERS([config.h])

dnl required version of automake
AM_INIT_AUTOMAKE([1.10])

dnl enable maintainer mode by default
AM_MAINTAINER_MODE([enable])

dnl check for tools (compiler etc.)
AC_PROG_CXX
AC_LANG([C++])

dnl required version of libtool  
LT_PREREQ([2.2.6])
LT_INIT

dnl error out if we can't find pkg-config
AC_CHECK_PROG(HAVE_PKGCONFIG, pkg-config, [ ], [
  AC_MSG_ERROR([You need to have pkg-config installed or set the PATH.])
])

dnl Check for required GStreamer modules
PKG_CHECK_MODULES(GST, [
    gstreamer-1.0              >= $GST_REQUIRED
    gstreamer-base-1.0         >= $GST_REQUIRED
    gstreamer-controller-1.0   >= $GST_REQUIRED
    gstreamer-plugins-base-1.0 >= $GST_REQUIRED
])

AC_SUBST(GST_CFLAGS)
AC_SUBST(GST_LIBS)

dnl check if compiler understands -Wall
AC_MSG_CHECKING([to see if compiler understands -Wall])
save_CXXFLAGS="$CXXFLAGS"
CXXFLAGS="$CXXFLAGS -Wall"
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([ ], [ ])], [
  CXXFLAGS="$CXXFLAGS -Wall"
  AC_MSG_RESULT([yes])
], [
  AC_MSG_RESULT([no])
])

dnl Create Makefiles
AC_CONFIG_FILES([
Makefile
src/Makefile  
])
AC_OUTPUT

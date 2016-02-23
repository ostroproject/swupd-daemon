#
# autoconf script for swupd-daemon
# Copyright (C) 2016 Intel Corporation
#
# Contact: Jussi Laako <jussi.laako@linux.intel.com>
#

AC_PREREQ([2.68])
AC_INIT([swupdd],
	[0.0.1],
	[jussi.laako@linux.intel.com],
	[swupdd])

AC_CONFIG_SRCDIR([src/swupdd-main.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([1.11 nostdinc silent-rules subdir-objects tar-pax dist-xz -Wno-portability])

AC_PROG_CC
AC_PROG_INSTALL


PKG_CHECK_MODULES(
		  [SWUPDD],
		  [gio-2.0 >= 2.32
		   gio-unix-2.0 >= 2.32
		   gobject-2.0 >= 2.32])
AC_SUBST(SWUPDD_CFLAGS)
AC_SUBST(SWUPDD_LIBS)

AC_CONFIG_FILES([
		 Makefile
		 src/Makefile
		 ])

AC_OUTPUT


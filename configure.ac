dnl configure.ac - Autoconf configuration for Spool.
dnl Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([spool],
        m4_esyscmd_s([cat version 2>/dev/null || echo "0.0.0-unknown"]),
        [thomasc1971@hotmail.com],
        [spool],
        [https://gitlab.gnome.org/])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([foreign subdir-objects -Wall -Werror])

dnl Recompute the version at configure time so a rebuild after new
dnl commits picks up the current version without re-running autoreconf.
SPOOL_VERSION=`"$srcdir/build-aux/git-version.sh" "$srcdir" 2>/dev/null`
AS_IF([test -z "$SPOOL_VERSION"],
      [SPOOL_VERSION="$PACKAGE_VERSION"])
AC_SUBST([SPOOL_VERSION])
AC_MSG_NOTICE([Spool version: $SPOOL_VERSION])

dnl Programs
AC_PROG_CC
AM_PROG_CC_C_O
AM_PROG_AR
AC_PROG_RANLIB

dnl glib-compile-resources for the embedded UI bundle.
AC_PATH_PROG([GLIB_COMPILE_RESOURCES], [glib-compile-resources])
AS_IF([test -z "$GLIB_COMPILE_RESOURCES"],
      [AC_MSG_ERROR([glib-compile-resources is required (install glib2-devel).])])

dnl Debug option (mirrors the bloom-* convention).
AC_ARG_ENABLE([debug],
  [AS_HELP_STRING([--enable-debug],
    [enable debug symbols and disable optimizations])],
  [enable_debug=$enableval],
  [enable_debug=no])

AS_IF([test "x$enable_debug" = "xyes"],
      [CFLAGS="-O0 -g3 -DDEBUG $CFLAGS"
       AC_MSG_NOTICE([Debug mode enabled: CFLAGS set to -O0 -g3 -DDEBUG])],
      [AS_IF([test "x$CFLAGS" = "x"], [CFLAGS="-O2"])])

dnl Wall + extra warnings useful in a small C codebase.
CFLAGS="$CFLAGS -Wall -Wextra -Wno-unused-parameter -Wmissing-prototypes"

dnl Required pkg-config packages.
PKG_CHECK_MODULES([GTK],       [gtk4 >= 4.10])
PKG_CHECK_MODULES([ADWAITA],   [libadwaita-1 >= 1.7])
PKG_CHECK_MODULES([GOA],       [goa-1.0 >= 3.40])
PKG_CHECK_MODULES([SOUP],      [libsoup-3.0 >= 3.0])
PKG_CHECK_MODULES([JSON_GLIB], [json-glib-1.0 >= 1.6])
PKG_CHECK_MODULES([GMIME],     [gmime-3.0 >= 3.2])
PKG_CHECK_MODULES([SQLITE],    [sqlite3 >= 3.24])
PKG_CHECK_MODULES([WEBKIT],    [webkitgtk-6.0 >= 2.44])
PKG_CHECK_MODULES([XML],       [libxml-2.0 >= 2.9])

dnl libetpan has no pkg-config file; probe it via AC_CHECK_LIB.
AC_CHECK_HEADER([libetpan/libetpan.h], [],
                [AC_MSG_ERROR([libetpan/libetpan.h not found (install libetpan-devel).])])
AC_CHECK_LIB([etpan], [mailimap_new], [LIBETPAN_LIBS="-letpan"],
             [AC_MSG_ERROR([libetpan does not export mailimap_new (install libetpan-devel).])])
AC_SUBST([LIBETPAN_LIBS])

dnl SPOOL_VERSION is intentionally NOT defined in config.h. src/Makefile.am
dnl regenerates src/spool-version.h on every `make` so the value tracks the
dnl current git HEAD without re-running configure. The SPOOL_VERSION shell
dnl variable above is still AC_SUBST'd so the Makefile rule has a fallback
dnl when build-aux/git-version.sh returns nothing (tarball builds, etc.).
AC_DEFINE([SPOOL_APP_ID], ["org.gnome.Spool"], [Application id])
AC_DEFINE([GOA_API_IS_SUBJECT_TO_CHANGE], [1], [Acknowledge libgoa's unstable API])

AC_CONFIG_FILES([
    Makefile
    src/Makefile
    data/Makefile
    tests/Makefile
])

AC_OUTPUT

AC_MSG_NOTICE([])
AC_MSG_NOTICE([  Spool $SPOOL_VERSION configured])
AC_MSG_NOTICE([    prefix:  $prefix])
AC_MSG_NOTICE([    CFLAGS:  $CFLAGS])
AC_MSG_NOTICE([    debug:   $enable_debug])
AC_MSG_NOTICE([])

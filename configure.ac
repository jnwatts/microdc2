#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

# Initialization
AC_PREREQ(2.59)
AC_INIT(microdc2, 0.15.6, [vladch@k804.mainet.msk.su])
AC_CONFIG_SRCDIR([src/microdc.h])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_MACRO_DIR([m4]) # doesn't seem to have any effect at the moment
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE
AC_FUNC_ALLOCA

# Initialize config.h
AC_GNU_SOURCE
AC_DEFINE_DIR(LOCALEDIR, datadir/locale, [Directory where gettext translations are kept.])

# Enable maintainer mode
AM_MAINTAINER_MODE

# Gnulib
gl_EARLY
gl_INIT

# Gettext
AM_GNU_GETTEXT([external])
# This is commented out, because with it, 'autoreconf -is' in autogen.sh will
# try to overwrite files in m4 from the gettext distribution. Unfortunately,
# at least in my case, gnulib provides newer files than the gettext distribution
# does.
# AM_GNU_GETTEXT_VERSION(0.14.3)

# Checks for programs
AC_PROG_CC
AC_PROG_RANLIB

# Checks for libraries
#ACX_PTHREAD
#ACX_PTHREAD([], [AC_MSG_ERROR([POSIX Threads is required to build and run this project.])])
AC_LIB_READLINE([], [AC_MSG_ERROR([GNU Readline is required to build and run this project.])])
### FIXME: move these to _rl_mark_modified_lines
AH_TEMPLATE([HAVE__RL_MARK_MODIFIED_LINES], [Define if _rl_mark_modified_lines exists in -lreadline.])
OLD_LIBS="$LIBS"
LIBS="$LIBS $READLINE_LIBS"
AC_CHECK_FUNC(_rl_mark_modified_lines, [AC_DEFINE([HAVE__RL_MARK_MODIFIED_LINES])])
LIBS="$OLD_LIBS"
AH_TEMPLATE([HAVE_LIBXML2], [Define if libxml2 is installed])
PKG_CHECK_MODULES(LIBXML2, libxml-2.0 > 2.6.16, AC_DEFINE([HAVE_LIBXML2]), AC_MSG_WARN([XML libxml2 library version greater than 2.6.16 is required to support XML filelists]))

# Checks for header files

# Checks for typedefs, structures, and compiler characteristics
AC_SYS_LARGEFILE
AC_CHECK_TYPES([comparison_fn_t])
AC_CHECK_MEMBERS(struct sigaction.sa_restorer,,,[#include <signal.h>])

# Checks for library functions

# Output generation
AC_CONFIG_FILES([Makefile
                 po/Makefile.in
		 lib/Makefile
		 src/Makefile
		 src/common/Makefile
		 src/bzip2/Makefile
		 src/tth/Makefile
		 rpm/microdc.spec])
AC_CONFIG_FILES([slackware/microdc.SlackBuild],
		[chmod +x slackware/microdc.SlackBuild])
AC_OUTPUT

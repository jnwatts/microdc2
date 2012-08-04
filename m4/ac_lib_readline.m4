dnl @synopsis AC_LIB_READLINE([ACTION-IF-TRUE], [ACTION-IF-FALSE])
dnl
dnl A modified version of VL_LIB_READLINE found in the GNU ac-archive
dnl (http://www.gnu.org/software/ac-archive) and the SF ac-archive
dnl (http://ac-archive.sourceforge.net/). Don't set LIBS, set
dnl READLINE_LIBS instead.
dnl
dnl Searches for the GNU Readline library.  If found, defines
dnl `HAVE_LIBREADLINE'.  If the found library has the `add_history'
dnl function, sets also `HAVE_READLINE_HISTORY'.  Also checks for the
dnl locations of the necessary include files and sets `HAVE_READLINE_H'
dnl or `HAVE_READLINE_READLINE_H' and `HAVE_READLINE_HISTORY_H' or
dnl 'HAVE_HISTORY_H' if the corresponding include files exists.
dnl
dnl The libraries that may be readline compatible are `libedit',
dnl `libeditline' and `libreadline'. However, this script does not check
dnl for those libraries. Sometimes we need to link a termcap library for
dnl readline to work, this macro tests these cases too by trying to link
dnl with `libtermcap', `libcurses' or `libncurses' before giving up.
dnl
dnl Here is an example of how to use the information provided by this
dnl macro to perform the necessary includes or declarations in a C file:
dnl
dnl  #if defined(HAVE_READLINE_READLINE_H)
dnl  # include <readline/readline.h>
dnl  #elif defined(HAVE_READLINE_H)
dnl  # include <readline.h>
dnl  #endif
dnl  #if defined(HAVE_READLINE_HISTORY_H)
dnl  # include <readline/history.h>
dnl  #elif defined(HAVE_HISTORY_H)
dnl  # include <history.h>
dnl  #endif
dnl
dnl TODO:
dnl   + --without-readline, --with-readline
dnl   + READLINE_CFLAGS so that #include <readline.h> is ok
dnl     even with includes in readline subdirectory?
dnl
dnl @originalversion 1.1
dnl @originalauthor Ville Laurikari <vl@iki.fi>
dnl @version 1.3
dnl @author Oskar Liljeblad <oskar@osk.mine.nu>
dnl
AC_DEFUN([AC_LIB_READLINE], [
  AH_TEMPLATE(HAVE_LIBREADLINE,
    [Define if you have a readline compatible library])
  AH_TEMPLATE(HAVE_READLINE_HISTORY,
    [Define if your readline library has `add_history'])
  AC_MSG_CHECKING([for the GNU Readline library])
  ORIG_LIBS="$LIBS"
  for readline_lib in readline; do
    for termcap_lib in "" termcap curses ncurses; do
      if test -z "$termcap_lib"; then
        TRY_LIB="-l$readline_lib"
      else
        TRY_LIB="-l$readline_lib -l$termcap_lib"
      fi
      LIBS="$ORIG_LIBS $TRY_LIB"
      AC_TRY_LINK_FUNC(readline, ol_cv_lib_readline="$TRY_LIB")
      if test -n "$ol_cv_lib_readline"; then
        break
      fi
    done
    if test -n "$ol_cv_lib_readline"; then
      break
    fi
  done
  if test -z "$ol_cv_lib_readline"; then
    AC_MSG_RESULT([not found])
    AC_SUBST(READLINE_LIBS, "")
    $2
  else
    AC_MSG_RESULT([found])
    AC_SUBST(READLINE_LIBS, "$ol_cv_lib_readline")
    AC_DEFINE(HAVE_LIBREADLINE, 1)
    AC_CHECK_HEADERS(readline.h readline/readline.h)
    ol_cv_lib_readline_history="no"
    AC_MSG_CHECKING([whether readline supports history])
    AC_TRY_LINK_FUNC(add_history, [
      AC_MSG_RESULT([yes])
      AC_DEFINE(HAVE_READLINE_HISTORY, 1)
      AC_CHECK_HEADERS(history.h readline/history.h)
    ], [
      AC_MSG_RESULT([no])
    ])
    $1
  fi
  LIBS="$ORIG_LIBS"
])dnl

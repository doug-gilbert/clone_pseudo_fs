AC_INIT(clone_pseudo_fs, 0.90, dgilbert@interlog.com)

AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AM_MAINTAINER_MODE
# AM_CONFIG_HEADER(config.h)
AC_CONFIG_HEADERS([config.h])

AC_LANG(C++)
AC_PROG_CXX
AC_PROG_INSTALL

AC_CANONICAL_HOST

# AM_PROG_AR is supported and needed since automake v1.12+
ifdef([AM_PROG_AR], [AM_PROG_AR], [])

AC_ARG_ENABLE([debug],
              [  --enable-debug          Turn on debugging],
              [case "${enableval}" in
                  yes) debug=true ;;
                  no)  debug=false ;;
                  *) AC_MSG_ERROR([bad value ${enableval} for --enable-debug]) ;;
               esac],[debug=false])
AM_CONDITIONAL([DEBUG], [test x$debug = xtrue])

AC_OUTPUT(Makefile src/Makefile doc/Makefile)

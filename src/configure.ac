#
# Copyright (c) 2019 Hirochika Asai <asai@jar.jp>
# All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#

AC_PREREQ(2.59)
AC_INIT([advfs], [0.0.1], [asai@jar.jp])
AC_CONFIG_SRCDIR([main.c])
AC_CONFIG_HEADER([config.h])

AM_INIT_AUTOMAKE
AC_SUBST(SUBDIRS)

# arguments
AC_ARG_ENABLE(debug,
  [  --enable-debug    Turn on debugging [default no]],
  [case "${enableval}" in
    yes) debug=yes; AC_MSG_RESULT(Checking for debug... yes); AC_DEFINE(DEBUG, 1, debugging option) ;;
    no)  debug=no;;
    *) AC_MSG_ERROR(bad value ${enableval} for --enable-debug) ;;
  esac],[debug=no])
AM_CONDITIONAL(DEBUG, test x$debug = xtrue)

# Checks for programs.
AC_PROG_CC
AC_PROG_INSTALL
#AC_PROG_LIBTOOL
#AC_CHECK_LIB

AC_DEFINE([_FILE_OFFSET_BITS], [64], [File Offset size])


AC_ARG_WITH(pkgconfigdir,
	[  --with-pkgconfigdir=DIR pkgconfig file in DIR @<:@LIBDIR/pkgconfig@:>@],
	[pkgconfigdir=$withval],
	[pkgconfigdir='${libdir}/pkgconfig'])
#AC_MSG_NOTICE([pkgconfig directory is ${pkgconfigdir}])
AC_SUBST(pkgconfigdir)

PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES(FUSE, [fuse >= 2.8])
AC_CHECK_HEADERS([fuse.h])

# Checks for libraries.
## OpenSSL
PKG_CHECK_MODULES(SSL, [openssl >= 1.0])

# Checks for header files.
AC_CHECK_HEADERS([stdlib.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_CHECK_MEMBERS([struct stat.st_birthtime])

# Checks for library functions.
#AC_CHECK_FUNCS([])

AC_CONFIG_FILES([Makefile fuse-advfs.pc])
AC_OUTPUT

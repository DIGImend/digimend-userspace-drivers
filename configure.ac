# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2021 Nikolai Kondrashov
#
# This file is part of digimend-userspace-drivers.

AC_PREREQ(2.61)
AC_INIT([digimend-userspace-drivers], [1])
AX_CHECK_ENABLE_DEBUG()
AM_INIT_AUTOMAKE([1.11 -Wall -Werror foreign])
AM_SILENT_RULES([yes])
AM_MAINTAINER_MODE
AC_USE_SYSTEM_EXTENSIONS
AX_COMPILER_FLAGS()
AC_CONFIG_HEADERS([config.h])

#
# Checks for libraries.
#
PKG_CHECK_MODULES(LIBUSB, libusb-1.0 >= 1.0.0)
CFLAGS="$CFLAGS $LIBUSB_CFLAGS"
LIBS="$LIBS $LIBUSB_LIBS"

#
# Checks for features
#
AC_ARG_ENABLE(
    tests-install,
    AS_HELP_STRING([--enable-tests-install], [enable installation of tests]),
    [], [enable_tests_install="no"])

#
# Output
#
AC_CONFIG_FILES([Makefile
                 include/Makefile
                 lib/Makefile
                 src/Makefile])
AC_OUTPUT

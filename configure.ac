dnl Process this file with autoconf to produce configure

dnl Copyright 2013 - 2016 Endless Mobile, Inc.

dnl This file is part of eos-event-recorder-daemon.
dnl
dnl eos-event-recorder-daemon is free software: you can redistribute it and/or
dnl modify it under the terms of the GNU General Public License as published by
dnl the Free Software Foundation, either version 2 of the License, or (at your
dnl option) any later version.
dnl
dnl eos-event-recorder-daemon is distributed in the hope that it will be
dnl useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
dnl MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
dnl Public License for more details.
dnl
dnl You should have received a copy of the GNU General Public License
dnl along with eos-event-recorder-daemon.  If not, see
dnl <http://www.gnu.org/licenses/>.

dnl ------------
dnl configure.ac
dnl ------------
dnl Please keep this file well-commented. Autotools involve a lot of magical
dnl incantations, and it is easy to mess things up if you don't know what you
dnl are doing.

# Initialization
# --------------

# Initialize Autoconf: package name, version, bug report address, tarball name,
# website
AC_INIT([EOS Event Recorder Daemon], [0.0.0],
    [], [eos-event-recorder-daemon], [http://endlessm.com])
# Verify that the source directory can be found
AC_CONFIG_SRCDIR([daemon/emer-daemon.h])
# Initialize Automake: enable all warnings and do not insist on GNU standards.
# no-portability suppresses warnings about syntax specific to GNU make.
# parallel-tests specifies that we use the new parallel-running test harness.
# Unlike serial-tests, this option is accepted by Automake 1.11.
# subdir-objects ensures forward compatibility with Automake 2.0 and can be
# removed when this behavior becomes the default.
# tar-ustar is required because there may be files whose entire paths exceed
# 99 characters.
AM_INIT_AUTOMAKE([-Wall -Wno-portability foreign 1.11 parallel-tests
    subdir-objects tar-ustar])
# Avoid spewing garbage over the terminal ('make V=1' to see the garbage)
AM_SILENT_RULES([yes])
# Keep Autotools macros local to this source tree
AC_CONFIG_MACRO_DIR([m4])
# Use Libtool
LT_INIT

AX_IS_RELEASE([git-directory])

AC_CACHE_SAVE

# Variables to define
# -------------------

# Required versions of libraries
# Update these whenever you use a function that requires a certain API version.
# Keep these lists sorted alphabetically.
EOS_METRICS_REQUIREMENT="eosmetrics-0 >= 0.2"
GIO_REQUIREMENT="gio-unix-2.0 >= 2.46"
GOBJECT_REQUIREMENT="gobject-2.0 >= 2.58"
OSTREE_REQUIREMENT="ostree-1 >= 2013.7"
POLKIT_REQUIREMENT="polkit-gobject-1"
SOUP_REQUIREMENT="libsoup-2.4 >= 2.42"
UUID_REQUIREMENT="uuid"
EMER_REQUIRED_MODULES="$EOS_METRICS_REQUIREMENT \
                       $GIO_REQUIREMENT \
                       $GOBJECT_REQUIREMENT \
                       $OSTREE_REQUIREMENT \
                       $POLKIT_REQUIREMENT \
                       $SOUP_REQUIREMENT \
                       $UUID_REQUIREMENT"
EMER_SHARED_REQUIRED_MODULES="$GIO_REQUIREMENT \
                              $GOBJECT_REQUIREMENT \
                              $UUID_REQUIREMENT"
AC_SUBST(EMER_REQUIRED_MODULES)
AC_SUBST(EMER_SHARED_REQUIRED_MODULES)

# Required build tools
# --------------------
# Make sure we can create directory hierarchies
AC_PROG_MKDIR_P
# C compiler
AC_PROG_CC
# Make sure the C compiler supports per-target CFLAGS
AC_PROG_CC_C_O
# Make sure the C compiler supports C99
AC_PROG_CC_C99
# Library configuration tool
PKG_PROG_PKG_CONFIG
# DBus code generation tool
AC_PATH_PROG([GDBUS_CODEGEN], [gdbus-codegen], [notfound])
AS_IF([test "x$GDBUS_CODEGEN" = "xnotfound"],
    [AC_MSG_ERROR([Could not find gdbus-codegen])])
AC_ARG_VAR([GDBUS_CODEGEN], [Path to gdbus-codegen])

AC_CACHE_SAVE


# Configure options
# -----------------
# --enable-compile-warnings. By default, warnings are always fatal. Documentation:
# https://www.gnu.org/software/autoconf-archive/ax_compiler_flags.html
#
# -Wno-declaration-after-statement:
#   Mixed decls are used a lot and are harmless.
AX_COMPILER_FLAGS([WARN_CFLAGS], [WARN_LDFLAGS], [no], [],
                  [-Wno-declaration-after-statement])

systemdsystemunitdir="$($PKG_CONFIG systemd --variable=systemdsystemunitdir)"
dnl Allow overriding systemdsystemunitdir during distcheck in order to trick
dnl Automake into allowing an install outside of $prefix
AC_ARG_WITH([systemdsystemunitdir],
    [AS_HELP_STRING([--with-systemdsystemunitdir=PATH], [directory for systemd service files])],
    [systemdsystemunitdir="$withval"])
AC_SUBST([systemdsystemunitdir])

EVENT_RECORDER_SERVER_XML="$($PKG_CONFIG eosmetrics-0 --variable=datadir)"/eos-metrics/com.endlessm.Metrics.xml
AC_SUBST(EVENT_RECORDER_SERVER_XML)

AC_MSG_CHECKING([for event recorder server XML description])
AS_IF([test -f $EVENT_RECORDER_SERVER_XML],
    [AC_MSG_RESULT([present])],
    [AC_MSG_ERROR([absent])])

AC_ARG_WITH([default_metrics_server],
    [AS_HELP_STRING([--with-default-metrics-server=HOSTNAME],[choose default metrics server])],
    [DEFAULT_METRICS_SERVER="\"$withval\""],
    [AC_MSG_ERROR([You must specify a default metrics server using --with-default-metrics-server])])
AC_DEFINE_UNQUOTED([DEFAULT_METRICS_SERVER], [$DEFAULT_METRICS_SERVER], [Default metrics server])

# Required libraries
# ------------------
PKG_CHECK_MODULES([EOS_EVENT_RECORDER_DAEMON], [$EMER_REQUIRED_MODULES])
PKG_CHECK_MODULES([EOS_EVENT_RECORDER_SHARED], [$EMER_SHARED_REQUIRED_MODULES])
# This indicates a required dbus mocking interface for unit testing.
AX_PYTHON_MODULE_VERSION([dbusmock], 0.10)

# The Python dbus module is required for unit testing.
AX_PYTHON_MODULE([dbus])

# Installed tests
# ---------------

AC_ARG_ENABLE([always_build_tests],
              AS_HELP_STRING([--enable-always-build-tests],
                             [Enable always building tests (default: yes)]),,
              [enable_always_build_tests=yes])
AC_ARG_ENABLE([installed_tests],
              AS_HELP_STRING([--enable-installed-tests],
                             [Install test programs (default: no)]),,
              [enable_installed_tests=no])

AM_CONDITIONAL([ENABLE_ALWAYS_BUILD_TESTS],
               [test "$enable_always_build_tests" = "yes"])
AC_SUBST([ENABLE_ALWAYS_BUILD_TESTS],[$enable_always_build_tests])

AM_CONDITIONAL([ENABLE_INSTALLED_TESTS],
               [test "$enable_installed_tests" = "yes"])
AC_SUBST([ENABLE_INSTALLED_TESTS],[$enable_installed_tests])

# Code coverage
# -------------
EOS_COVERAGE_REPORT([c])

AC_CACHE_SAVE

# Output
# ------
# List files here that the configure script should output
AC_CONFIG_FILES([
    Makefile
])
AC_CONFIG_HEADERS([config.h])  dnl Header with system-dependent #defines
AC_OUTPUT

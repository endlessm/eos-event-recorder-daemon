dnl Process this file with autoconf to produce configure

# Copyright 2013-2014 Endless Mobile, Inc.

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
# Initialize Automake: enable all warnings and do not insist on GNU standards
# no-portability suppresses warnings about syntax specific to GNU make
# parallel-tests specifies that we use the new parallel-running test harness.
# Unlike serial-tests, this option is accepted by Automake 1.11
# tar-ustar is required because there may be files whose entire paths exceed
# 99 characters.
AM_INIT_AUTOMAKE([-Wall -Wno-portability foreign 1.11 parallel-tests tar-ustar])
# Avoid spewing garbage over the terminal ('make V=1' to see the garbage)
AM_SILENT_RULES([yes])
# Keep Autotools macros local to this source tree
AC_CONFIG_MACRO_DIR([m4])

AC_CACHE_SAVE

# Variables to define
# -------------------

# Required versions of libraries
# Update these whenever you use a function that requires a certain API version.
# Keep these lists sorted alphabetically.
EOS_METRICS_REQUIREMENT="eosmetrics-0 >= 0.2"
GIO_REQUIREMENT="gio-unix-2.0 >= 2.40"
GOBJECT_REQUIREMENT="gobject-2.0"
POLKIT_REQUIREMENT="polkit-gobject-1"
SOUP_REQUIREMENT="libsoup-2.4 >= 2.42"
UUID_REQUIREMENT="uuid"
EMER_REQUIRED_MODULES="$EOS_METRICS_REQUIREMENT $GIO_REQUIREMENT $GOBJECT_REQUIREMENT $POLKIT_REQUIREMENT $SOUP_REQUIREMENT $UUID_REQUIREMENT"
AC_SUBST(EMER_REQUIRED_MODULES)
# This indicates a required dbus mocking interface for unit testing.
AX_PYTHON_MODULE([dbusmock], [required])

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
# --enable-strict-flags: Compile with strict compiler flags. Done automatically
# during 'make distcheck'.
AC_ARG_ENABLE([strict-flags],
    [AS_HELP_STRING([--enable-strict-flags=@<:@no/yes/error@:>@],
        [Use strict compiler flags @<:@default=no@:>@])],
    [],
    [enable_strict_flags=no])
# Emmanuele's list of flags
STRICT_COMPILER_FLAGS="$STRICT_COMPILER_FLAGS
    -Wall
    -Wcast-align
    -Wuninitialized
    -Wno-strict-aliasing
    -Werror=pointer-arith
    -Werror=missing-declarations
    -Werror=redundant-decls
    -Werror=empty-body
    -Werror=format
    -Werror=format-security
    -Werror=format-nonliteral
    -Werror=init-self"
AS_CASE([$enable_strict_flags],
    [yes],
        [AS_COMPILER_FLAGS([STRICT_CFLAGS], [$STRICT_COMPILER_FLAGS])],
    [no],
        [],
        [error],
        [
            STRICT_COMPILER_FLAGS="$STRICT_COMPILER_FLAGS -Werror"
            AS_COMPILER_FLAGS([STRICT_CFLAGS], [$STRICT_COMPILER_FLAGS])
        ],
    [AC_MSG_ERROR([Invalid option for --enable-strict-flags])])
dnl Strip leading spaces
STRICT_CFLAGS=${STRICT_CFLAGS#*  }
AC_SUBST(STRICT_CFLAGS)

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

# Required libraries
# ------------------
PKG_CHECK_MODULES([EOS_EVENT_RECORDER_DAEMON], [$EMER_REQUIRED_MODULES])

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

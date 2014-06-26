## Process this file with automake to produce Makefile.in

# Copyright 2014 Endless Mobile, Inc.

## -----------
## Makefile.am
## -----------
## Please keep this file well-commented.

ACLOCAL_AMFLAGS = -I m4

# Extra files to distribute in the tarball
EXTRA_DIST = README.md

# Compiler flags
AM_CFLAGS = @STRICT_CFLAGS@

# Make sure to use strict compiler flags when doing 'make distcheck'
AM_DISTCHECK_CONFIGURE_FLAGS = --enable-strict-flags

# Generated files that 'make clean' should erase
CLEANFILES =
DISTCLEANFILES =

# # # ANALYTICS # # #

libexec_PROGRAMS = eos-metric-parser

eos_metric_parser_SOURCES = src/metric_request_parser.c

# Pre-processor flags
AM_CPPFLAGS = $(EOS_ANALYTICS_CFLAGS)

# Libraries to link against
eos_metric_parser_LDADD = $(EOS_ANALYTICS_LIBS)

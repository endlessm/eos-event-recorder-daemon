## Process this file with automake to produce Makefile.in

# Copyright 2013 Endless Mobile, Inc.

## -----------
## Makefile.am
## -----------
## Please keep this file well-commented.

# Subdirectories where we also want to run make
# (Make sure that the library is built before the docs)
SUBDIRS = . docs/reference/eosmetrics

# Our Autoconf macros are kept in the m4/ directory
ACLOCAL_AMFLAGS = -I m4

# Extra files to distribute in the tarball
EXTRA_DIST = README.md @EMTR_API_NAME@.pc.in

# Compiler flags
AM_CFLAGS = @STRICT_CFLAGS@

# Make sure to run Gtk-doc tests and build the documentation when doing
# 'make distcheck'
AM_DISTCHECK_CONFIGURE_FLAGS = \
	--enable-gtk-doc \
	--enable-gir-doc \
	--enable-strict-flags \
	--with-systemdsystemunitdir=$(libdir)/systemd \
	$(NULL)

# Generated files that 'make clean' should erase
CLEANFILES =
DISTCLEANFILES =

# Generated sources that should be built before other sources
BUILT_SOURCES =

# Make sure that 'make dist' includes documentation
if CAN_MAKE_DIST
dist-hook:
else
dist-hook:
	@echo "***"
	@echo "*** You must configure with --enable-gtk-doc and --enable-gir-doc to"
	@echo "*** run make dist or make distcheck."
	@echo "***"
	@false
endif

persistentcachedir=$(localstatedir)/cache/metrics/
permissionsdir=$(sysconfdir)/metrics/
permissions_file=$(permissionsdir)eos-metrics-permissions.conf

# chown and chgrp are not allowed unless you are root or under some specific
# other conditions that may not be met every time you run 'make'.
install-data-hook:
	$(MKDIR_P) --mode=g+s,ug+rwx,a+r $(DESTDIR)$(persistentcachedir) $(DESTDIR)$(permissionsdir)
	chmod --recursive g+s,ug+rwx,a+r $(DESTDIR)$(persistentcachedir) $(DESTDIR)$(permissionsdir)
	@if id -u metrics 2> /dev/null; then \
		if ! chown --recursive metrics:metrics $(DESTDIR)$(persistentcachedir) $(DESTDIR)$(permissionsdir); then \
			echo "*************************************"; \
			echo "Make sure to recursively change the owner and"; \
			echo "group of the $(persistentcachedir) and the" ; \
			echo "$(permissionsdir) directories to 'metrics'."; \
			echo "*************************************"; \
		fi; \
	else \
		echo "*******************************************"; \
		echo "There is no 'metrics' user on your system."; \
		echo "If this is an EndlessOS system, upgrade the"; \
		echo "'base-passwd' package."; \
		echo "Otherwise, create a 'metrics' user."; \
		echo "*******************************************"; \
	fi

uninstall-hook:
	rm -rf $(DESTDIR)$(persistentcachedir) $(DESTDIR)$(permissionsdir)

# # # DATA # # #

include $(top_srcdir)/data/Makefile.am.inc

# # # LIBRARY # # #

# Metrics library
include $(top_srcdir)/eosmetrics/Makefile.am.inc

# Public header files
eosmetricsincludedir = $(includedir)/@EMTR_API_NAME@
nobase_eosmetricsinclude_HEADERS = \
	$(eosmetrics_public_installed_headers) \
	$(eosmetrics_private_installed_headers) \
	$(NULL)

# Pkg-config file
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = @EMTR_API_NAME@.pc
DISTCLEANFILES += @EMTR_API_NAME@.pc

# # # DBUS DAEMON ("EVENT RECORDER SERVER") # # #

include $(top_srcdir)/daemon/Makefile.am.inc

# # # INTROSPECTION FILES # # #

-include $(INTROSPECTION_MAKEFILE)
INTROSPECTION_GIRS =
INTROSPECTION_SCANNER_ARGS = --add-include-path=$(srcdir) --warn-all
INTROSPECTION_COMPILER_ARGS = --includedir=$(srcdir)

if HAVE_INTROSPECTION
introspection_sources = \
	$(filter-out %.h, $(eosmetrics_library_sources)) \
	$(eosmetrics_public_installed_headers) \
	$(eosmetrics_private_installed_headers) \
	$(NULL)

EosMetrics-@EMTR_API_VERSION@.gir: libeosmetrics-@EMTR_API_VERSION@.la
EosMetrics_@EMTR_API_VERSION@_gir_INCLUDES = \
	GObject-2.0 \
	GLib-2.0 \
	Gio-2.0 \
	$(NULL)
EosMetrics_@EMTR_API_VERSION@_gir_SCANNERFLAGS = \
	--identifier-prefix=Emtr \
	--symbol-prefix=emtr \
	-DCOMPILING_EOS_METRICS \
	$(NULL)
EosMetrics_@EMTR_API_VERSION@_gir_LIBS = libeosmetrics-@EMTR_API_VERSION@.la
EosMetrics_@EMTR_API_VERSION@_gir_FILES = $(introspection_sources)
EosMetrics_@EMTR_API_VERSION@_gir_EXPORT_PACKAGES = @EMTR_API_NAME@
INTROSPECTION_GIRS += EosMetrics-@EMTR_API_VERSION@.gir

girdir = $(datadir)/gir-1.0
gir_DATA = $(INTROSPECTION_GIRS)

typelibdir = $(libdir)/girepository-1.0
typelib_DATA = $(INTROSPECTION_GIRS:.gir=.typelib)

CLEANFILES += $(gir_DATA) $(typelib_DATA)
endif

# # # GOBJECT INTROSPECTION DOCUMENTATION # # #

if ENABLE_GIR_DOC

docs/reference/eosmetrics-js/index.page: $(INTROSPECTION_GIRS)
	$(MKDIR_P) $(@D)
	g-ir-doc-tool -o $(@D) -l gjs $<

docs/reference/eosmetrics-js/html/index.html: docs/reference/eosmetrics-js/index.page
	$(MKDIR_P) $(@D)
	yelp-build html -o $(@D) $(<D)

all-local: docs/reference/eosmetrics-js/html/index.html

EXTRA_DIST += \
	docs/reference/eosmetrics-js/html/*.html \
	docs/reference/eosmetrics-js/html/*.css \
	docs/reference/eosmetrics-js/html/*.js \
	$(NULL)

CLEANFILES += \
	docs/reference/eosmetrics-js/*.page \
	docs/reference/eosmetrics-js/html/*.html \
	docs/reference/eosmetrics-js/html/*.css \
	docs/reference/eosmetrics-js/html/*.js \
	$(NULL)
endif

# # # TOOLS # # #

include $(top_srcdir)/tools/Makefile.am.inc

# # # TESTS # # #

include $(top_srcdir)/tests/Makefile.am.inc

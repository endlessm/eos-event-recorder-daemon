## Process this file with automake to produce Makefile.in

## Copyright 2013, 2014, 2015 Endless Mobile, Inc.

## This file is part of eos-event-recorder-daemon.
##
## eos-event-recorder-daemon is free software: you can redistribute it and/or
## modify it under the terms of the GNU General Public License as published by
## the Free Software Foundation, either version 2 of the License, or (at your
## option) any later version.
##
## eos-event-recorder-daemon is distributed in the hope that it will be
## useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
## Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with eos-event-recorder-daemon.  If not, see
## <http://www.gnu.org/licenses/>.

## -----------
## Makefile.am
## -----------
## Please keep this file well-commented.

include $(top_srcdir)/glib-tap.mk

# Our Autoconf macros are kept in the m4/ directory
ACLOCAL_AMFLAGS = -I m4

# Extra files to distribute in the tarball
EXTRA_DIST += \
	glib-tap.mk \
	README.md \
	tap-driver.sh \
	tap-test \
	$(NULL)

# Compiler flags
AM_CFLAGS = @WARN_CFLAGS@
AM_LDFLAGS = @WARN_LDFLAGS@

# Enable strict compiler flags and install systemd unit files to the specified
# directory when doing 'make distcheck'.
AM_DISTCHECK_CONFIGURE_FLAGS = \
	--enable-compile-warnings=error \
	--with-systemdsystemunitdir='$${prefix}/lib/systemd/system' \
	$(NULL)
if EOS_ENABLE_COVERAGE
AM_DISTCHECK_CONFIGURE_FLAGS += --enable-coverage --with-coverage-dir=@EOS_COVERAGE_DIR@
endif EOS_ENABLE_COVERAGE

# Generated files that 'make clean' should erase
CLEANFILES +=
DISTCLEANFILES +=

# Generated sources that should be built before other sources
BUILT_SOURCES +=

# Binaries that should be produced but not installed.
noinst_PROGRAMS +=

# Libraries that should be produced but not installed.
noinst_LTLIBRARIES +=

# Binaries that should be installed in libexecdir
libexec_PROGRAMS =

substitute_libexecdir = sed \
	-e 's|@libexecdir[@]|$(libexecdir)|g' \
	$(NULL)

# dbus services
dbusservicedir = ${datadir}/dbus-1/system-services
dist_dbusservice_DATA =

# dbus security policy
systembussecuritypolicydir = ${sysconfdir}/dbus-1/system.d
dist_systembussecuritypolicy_DATA =

# policykit
policydir = ${datadir}/polkit-1/actions
dist_policy_DATA =

policyrulesdir = $(datadir)/polkit-1/rules.d
dist_policyrules_DATA =

persistentcachedir=$(localstatedir)/cache/metrics/
configdir=$(sysconfdir)/metrics/
permissions_file=$(configdir)eos-metrics-permissions.conf

# systemd
systemdsystemunit_DATA =

# chown and chgrp are not allowed unless you are root or under some specific
# other conditions that may not be met every time you run 'make'.
install-data-hook:
	$(MKDIR_P) --mode=g+s,ug+rwx,a+rx $(DESTDIR)$(persistentcachedir) $(DESTDIR)$(configdir)
	chmod --recursive g+s,ug+rwx,a+rx $(DESTDIR)$(persistentcachedir) $(DESTDIR)$(configdir)
	@if id -u metrics 2> /dev/null; then \
		if ! chown --recursive metrics:metrics $(DESTDIR)$(persistentcachedir) $(DESTDIR)$(configdir); then \
			echo "*************************************"; \
			echo "Make sure to recursively change the owner and"; \
			echo "group of the $(persistentcachedir) and the" ; \
			echo "$(configdir) directories to 'metrics'."; \
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
	rm -rf $(DESTDIR)$(persistentcachedir) $(DESTDIR)$(configdir)

# # # DATA # # #

include $(top_srcdir)/data/Makefile.am.inc

# # # CODE COVERAGE # # #

EOS_COVERAGE_BLACKLIST_PATTERNS =	\
	*/tests/*			\
	*emer-event-recorder-server.c	\
	$(NULL)

@EOS_COVERAGE_RULES@

AM_CFLAGS += @EOS_C_COVERAGE_CFLAGS@
AM_LDFLAGS += @EOS_C_COVERAGE_LDFLAGS@

# # # COMMON LIBRARY # # #
include $(top_srcdir)/shared/Makefile.am.inc

# # # DBUS DAEMON ("EVENT RECORDER SERVER") # # #

include $(top_srcdir)/daemon/Makefile.am.inc

# # # TOOLS # # #

include $(top_srcdir)/tools/Makefile.am.inc

# # # TESTS # # #

include $(top_srcdir)/tests/Makefile.am.inc

clean-local:: clean-coverage

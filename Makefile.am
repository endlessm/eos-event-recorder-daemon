## Process this file with automake to produce Makefile.in

# Copyright 2013-2014 Endless Mobile, Inc.

## -----------
## Makefile.am
## -----------
## Please keep this file well-commented.

# Our Autoconf macros are kept in the m4/ directory
ACLOCAL_AMFLAGS = -I m4

# Extra files to distribute in the tarball
EXTRA_DIST = README.md

# Compiler flags
AM_CFLAGS = @STRICT_CFLAGS@

# Enable strict compiler flags and install systemd unit files to the specified
# directory when doing 'make distcheck'.
AM_DISTCHECK_CONFIGURE_FLAGS = \
	--enable-strict-flags \
	--with-systemdsystemunitdir=$(libdir)/systemd \
	$(NULL)

# Generated files that 'make clean' should erase
CLEANFILES =
DISTCLEANFILES =

# Generated sources that should be built before other sources
BUILT_SOURCES =

# Binaries that should be produced but not installed.
noinst_PROGRAMS =

persistentcachedir=$(localstatedir)/cache/metrics/
permissionsdir=$(sysconfdir)/metrics/
permissions_file=$(permissionsdir)eos-metrics-permissions.conf

# chown and chgrp are not allowed unless you are root or under some specific
# other conditions that may not be met every time you run 'make'.
install-data-hook:
	$(MKDIR_P) --mode=g+s,ug+rwx,a+rx $(DESTDIR)$(persistentcachedir) $(DESTDIR)$(permissionsdir)
	chmod --recursive g+s,ug+rwx,a+rx $(DESTDIR)$(persistentcachedir) $(DESTDIR)$(permissionsdir)
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

# # # DBUS DAEMON ("EVENT RECORDER SERVER") # # #

include $(top_srcdir)/daemon/Makefile.am.inc

# # # TOOLS # # #

include $(top_srcdir)/tools/Makefile.am.inc

# # # TESTS # # #

include $(top_srcdir)/tests/Makefile.am.inc

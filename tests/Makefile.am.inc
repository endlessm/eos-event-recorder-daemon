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

# Actually included at the top of $(top_srcdir)/Makefile.am:
# include $(top_srcdir)/glib-tap.mk

installed_testdir = $(libexecdir)/installed-tests/$(PACKAGE_TARNAME)
installed_test_metadir = $(datadir)/installed-tests/$(PACKAGE_TARNAME)

DAEMON_TEST_FLAGS = \
	@EOS_EVENT_RECORDER_DAEMON_CFLAGS@ \
	-I$(top_srcdir) \
	-I$(top_srcdir)/daemon \
	-DCONFIG_DIR="\"$(configdir)\"" \
	-DPERMISSIONS_FILE="\"$(permissions_file)\"" \
	-DPERSISTENT_CACHE_DIR="\"$(persistentcachedir)\"" \
	-DSYSCONFDIR="\"$(sysconfdir)\"" \
	-DTEST_DIR="\"$(srcdir)/tests/\"" \
	$(NULL)
DAEMON_TEST_LIBS = \
	@EOS_EVENT_RECORDER_DAEMON_LIBS@ \
	$(top_builddir)/shared/libeos-metrics-event-recorder-shared.la \
	-lm

tests_daemon_test_aggregate_tally_SOURCES = \
	daemon/emer-aggregate-tally.c daemon/emer-aggregate-tally.h \
	shared/metrics-util.c shared/metrics-util.h \
	tests/daemon/test-aggregate-tally.c \
	$(NULL)
tests_daemon_test_aggregate_tally_CPPFLAGS = $(DAEMON_TEST_FLAGS)
tests_daemon_test_aggregate_tally_LDADD = $(DAEMON_TEST_LIBS)

tests_daemon_test_boot_id_provider_SOURCES = \
	daemon/emer-boot-id-provider.c daemon/emer-boot-id-provider.h \
	tests/daemon/test-boot-id-provider.c \
	$(NULL)
tests_daemon_test_boot_id_provider_CPPFLAGS = $(DAEMON_TEST_FLAGS)
tests_daemon_test_boot_id_provider_LDADD = $(DAEMON_TEST_LIBS)

tests_daemon_test_cache_size_provider_SOURCES = \
	daemon/emer-cache-size-provider.c daemon/emer-cache-size-provider.h \
	tests/daemon/test-cache-size-provider.c \
	$(NULL)
tests_daemon_test_cache_size_provider_CPPFLAGS = $(DAEMON_TEST_FLAGS)
tests_daemon_test_cache_size_provider_LDADD = $(DAEMON_TEST_LIBS)

tests_daemon_test_cache_version_provider_SOURCES = \
	daemon/emer-cache-version-provider.c daemon/emer-cache-version-provider.h \
	tests/daemon/test-cache-version-provider.c \
	$(NULL)
tests_daemon_test_cache_version_provider_CPPFLAGS = $(DAEMON_TEST_FLAGS)
tests_daemon_test_cache_version_provider_LDADD = $(DAEMON_TEST_LIBS)

tests_daemon_test_circular_file_SOURCES = \
	tests/daemon/test-circular-file.c \
	daemon/emer-circular-file.c daemon/emer-circular-file.h \
	shared/metrics-util.c shared/metrics-util.h \
	$(NULL)
tests_daemon_test_circular_file_CPPFLAGS = $(DAEMON_TEST_FLAGS)
tests_daemon_test_circular_file_LDADD = $(DAEMON_TEST_LIBS)

tests_daemon_test_daemon_dbusdaemon_SOURCES = \
	emer-event-recorder-server.c emer-event-recorder-server.h \
	daemon/eins-boottime-source.c daemon/eins-boottime-source.h \
	daemon/emer-aggregate-tally.c daemon/emer-aggregate-tally.h \
	daemon/emer-aggregate-timer-impl.c daemon/emer-aggregate-timer-impl.h \
	daemon/emer-daemon.c daemon/emer-daemon.h \
	tests/daemon/test-daemon.c \
	daemon/emer-boot-id-provider.c daemon/emer-boot-id-provider.h \
	tests/daemon/mock-cache-size-provider.c daemon/emer-cache-size-provider.h \
	daemon/emer-gzip.c daemon/emer-gzip.h \
	tests/daemon/mock-image-id-provider.c tests/daemon/mock-image-id-provider.h \
	tests/daemon/mock-machine-id-provider.c daemon/emer-machine-id-provider.h \
	tests/daemon/mock-network-send-provider.c \
	daemon/emer-network-send-provider.h \
	tests/daemon/mock-permissions-provider.c \
	tests/daemon/mock-permissions-provider.h \
	daemon/emer-permissions-provider.h \
	tests/daemon/mock-persistent-cache.c tests/daemon/mock-persistent-cache.h \
	tests/daemon/mock-site-id-provider.c daemon/emer-site-id-provider.h \
	daemon/emer-types.c daemon/emer-types.h \
	$(NULL)
tests_daemon_test_daemon_dbusdaemon_CPPFLAGS = $(DAEMON_TEST_FLAGS)
tests_daemon_test_daemon_dbusdaemon_LDADD = $(DAEMON_TEST_LIBS)

tests_daemon_test_gzip_SOURCES = \
	daemon/emer-gzip.c daemon/emer-gzip.h \
	tests/daemon/test-gzip.c \
	$(NULL)
tests_daemon_test_gzip_CPPFLAGS = $(DAEMON_TEST_FLAGS)
tests_daemon_test_gzip_LDADD = $(DAEMON_TEST_LIBS)

tests_daemon_test_machine_id_provider_SOURCES = \
	daemon/emer-types.c daemon/emer-types.h \
	daemon/emer-machine-id-provider.c daemon/emer-machine-id-provider.h \
	tests/daemon/test-machine-id-provider.c \
	$(NULL)
tests_daemon_test_machine_id_provider_CPPFLAGS = $(DAEMON_TEST_FLAGS)
tests_daemon_test_machine_id_provider_LDADD = $(DAEMON_TEST_LIBS)

tests_daemon_test_network_send_provider_SOURCES = \
	daemon/emer-network-send-provider.c daemon/emer-network-send-provider.h \
	tests/daemon/test-network-send-provider.c \
	$(NULL)
tests_daemon_test_network_send_provider_CPPFLAGS = $(DAEMON_TEST_FLAGS)
tests_daemon_test_network_send_provider_LDADD = $(DAEMON_TEST_LIBS)

tests_daemon_test_permissions_provider_SOURCES = \
	tests/daemon/test-permissions-provider.c \
	daemon/emer-permissions-provider.c daemon/emer-permissions-provider.h \
	$(NULL)
tests_daemon_test_permissions_provider_CPPFLAGS = $(DAEMON_TEST_FLAGS)
tests_daemon_test_permissions_provider_LDADD = $(DAEMON_TEST_LIBS)

tests_daemon_test_persistent_cache_SOURCES = \
	tests/daemon/test-persistent-cache.c \
	daemon/emer-persistent-cache.c daemon/emer-persistent-cache.h \
	daemon/emer-boot-id-provider.c daemon/emer-boot-id-provider.h \
	tests/daemon/mock-cache-version-provider.c \
	daemon/emer-cache-version-provider.h \
	tests/daemon/mock-circular-file.c tests/daemon/mock-circular-file.h \
	daemon/emer-circular-file.h \
	shared/metrics-util.c shared/metrics-util.h \
	$(NULL)
tests_daemon_test_persistent_cache_CPPFLAGS = $(DAEMON_TEST_FLAGS)
tests_daemon_test_persistent_cache_LDADD = $(DAEMON_TEST_LIBS)

dist_test_data = \
	tests/daemon/mock-server.py \
	tests/launch-mock-dbus-tests.sh \
	tests/taptestrunner.py \
	$(NULL)

# Run tests when running 'make check'
test_programs = \
	tests/daemon/test-aggregate-tally \
	tests/daemon/test-boot-id-provider \
	tests/daemon/test-cache-size-provider \
	tests/daemon/test-cache-version-provider \
	tests/daemon/test-circular-file \
	tests/daemon/test-daemon.dbusdaemon \
	tests/daemon/test-gzip \
	tests/daemon/test-machine-id-provider \
	tests/daemon/test-network-send-provider \
	tests/daemon/test-persistent-cache \
	tests/daemon/test-permissions-provider \
	$(NULL)
dist_test_scripts = \
	tests/test-opt-out-integration.py \
	$(NULL)

TEST_EXTENSIONS = .dbusdaemon .py
DBUSDAEMON_LOG_COMPILER = $(top_srcdir)/tests/launch-mock-dbus-tests.sh
PY_LOG_COMPILER = python3

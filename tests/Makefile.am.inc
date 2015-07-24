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

noinst_PROGRAMS += \
	tests/daemon/test-boot-id-provider \
	tests/daemon/test-cache-size-provider \
	tests/daemon/test-cache-version-provider \
	tests/daemon/test-circular-file \
	tests/daemon/test-daemon.dbusdaemon \
	tests/daemon/test-machine-id-provider \
	tests/daemon/test-network-send-provider \
	tests/daemon/test-permissions-provider \
	tests/daemon/test-persistent-cache \
	$(NULL)

DAEMON_TEST_FLAGS = \
	@EOS_EVENT_RECORDER_DAEMON_CFLAGS@ \
	-I$(top_srcdir) \
	-I$(top_srcdir)/daemon \
	-DCONFIG_DIR="\"$(configdir)\"" \
	-DPERMISSIONS_FILE="\"$(permissions_file)\"" \
	-DPERSISTENT_CACHE_DIR="\"$(persistentcachedir)\"" \
	-DTEST_DIR="\"$(srcdir)/tests/\"" \
	$(NULL)
DAEMON_TEST_LIBS = @EOS_EVENT_RECORDER_DAEMON_LIBS@

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
	daemon/emer-daemon.c daemon/emer-daemon.h \
	tests/daemon/test-daemon.c \
	daemon/emer-boot-id-provider.c daemon/emer-boot-id-provider.h \
	daemon/emer-cache-size-provider.c daemon/emer-cache-size-provider.h \
	daemon/emer-cache-version-provider.c daemon/emer-cache-version-provider.h \
	tests/daemon/mock-machine-id-provider.c daemon/emer-machine-id-provider.h \
	tests/daemon/mock-network-send-provider.c \
	daemon/emer-network-send-provider.h \
	tests/daemon/mock-permissions-provider.c \
	tests/daemon/mock-permissions-provider.h \
	daemon/emer-permissions-provider.h \
	tests/daemon/mock-persistent-cache.c tests/daemon/mock-persistent-cache.h \
	shared/metrics-util.c shared/metrics-util.h \
	$(NULL)
tests_daemon_test_daemon_dbusdaemon_CPPFLAGS = $(DAEMON_TEST_FLAGS)
tests_daemon_test_daemon_dbusdaemon_LDADD = $(DAEMON_TEST_LIBS)

tests_daemon_test_machine_id_provider_SOURCES = \
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
	daemon/emer-cache-size-provider.c daemon/emer-cache-size-provider.h \
	daemon/emer-cache-version-provider.c daemon/emer-cache-version-provider.h \
	tests/daemon/mock-circular-file.c daemon/emer-circular-file.h \
	shared/metrics-util.c shared/metrics-util.h \
	$(NULL)
tests_daemon_test_persistent_cache_CPPFLAGS = $(DAEMON_TEST_FLAGS)
tests_daemon_test_persistent_cache_LDADD = $(DAEMON_TEST_LIBS)

dist_noinst_SCRIPTS = \
	tests/daemon/mock-server.py \
	tests/launch-mock-dbus-tests.sh \
	$(NULL)

# Run tests when running 'make check'
TESTS = \
	tests/test-opt-out-integration.py \
	tests/daemon/test-boot-id-provider \
	tests/daemon/test-cache-size-provider \
	tests/daemon/test-cache-version-provider \
	tests/daemon/test-circular-file \
	tests/daemon/test-daemon.dbusdaemon \
	tests/daemon/test-machine-id-provider \
	tests/daemon/test-network-send-provider \
	tests/daemon/test-persistent-cache \
	tests/daemon/test-permissions-provider \
	run_coverage.coverage \
	$(NULL)

TEST_EXTENSIONS = .dbusdaemon .py .coverage
DBUSDAEMON_LOG_COMPILER = $(top_srcdir)/tests/launch-mock-dbus-tests.sh
AM_DBUSDAEMON_LOG_FLAGS = -k --verbose
LOG_COMPILER = gtester
AM_LOG_FLAGS = -k --verbose
PY_LOG_COMPILER = python3

EXTRA_DIST += \
	tests/test-opt-out-integration.py \
	$(NULL)

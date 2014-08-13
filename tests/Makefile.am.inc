# Copyright 2013 Endless Mobile, Inc.

noinst_PROGRAMS = \
	tests/test-event-types \
	tests/test-library.dbuseventrecorder \
	tests/daemon/test-boot-id-provider \
    tests/daemon/test-cache-version-provider \
	tests/daemon/test-daemon.dbusdaemon \
	tests/daemon/test-machine-id-provider \
	tests/daemon/test-network-send-provider \
	tests/daemon/test-permissions-provider \
	tests/daemon/test-persistent-cache \
	$(NULL)

LIBRARY_TEST_FLAGS = \
	@EOSMETRICS_CFLAGS@ \
	-I$(top_srcdir) \
	-DCOMPILING_EOS_METRICS \
	-D_POSIX_C_SOURCE=200112L \
	$(NULL)
LIBRARY_TEST_LIBS = \
	@EOSMETRICS_LIBS@ \
	$(top_builddir)/libeosmetrics-@EMTR_API_VERSION@.la \
	$(NULL)

tests_test_library_dbuseventrecorder_SOURCES = tests/test-event-recorder.c
tests_test_library_dbuseventrecorder_CPPFLAGS = $(LIBRARY_TEST_FLAGS)
tests_test_library_dbuseventrecorder_LDADD = $(LIBRARY_TEST_LIBS)

EOSMETRICS_TEST_FLAGS = \
	@EOSMETRICS_EVENT_RECORDER_CFLAGS@ \
	-I$(top_srcdir) \
	$(NULL)
EOSMETRICS_TEST_LIBS = @EOSMETRICS_EVENT_RECORDER_LIBS@

tests_test_event_types_SOURCES = \
	eosmetrics/emtr-event-types.c eosmetrics/emtr-event-types.h \
	eosmetrics/eosmetrics.h \
	tests/test-event-types.c \
	$(NULL)
tests_test_event_types_CPPFLAGS = $(EOSMETRICS_TEST_FLAGS)
tests_test_event_types_LDADD = $(EOSMETRICS_TEST_LIBS)

DAEMON_TEST_FLAGS = \
	@EOSMETRICS_EVENT_RECORDER_CFLAGS@ \
	-I$(top_srcdir) \
	-I$(top_srcdir)/daemon \
	-DPERSISTENT_CACHE_DIR="\"$(persistentcachedir)\"" \
	-DPERMISSIONS_FILE="\"$(permissions_file)\"" \
	$(NULL)
DAEMON_TEST_LIBS = @EOSMETRICS_EVENT_RECORDER_LIBS@

tests_daemon_test_cache_version_provider_SOURCES = \
	daemon/emer-cache-version-provider.c daemon/emer-cache-version-provider.h \
	tests/daemon/test-cache-version-provider.c \
	$(NULL)
tests_daemon_test_cache_version_provider_CPPFLAGS = $(DAEMON_TEST_FLAGS)
tests_daemon_test_cache_version_provider_LDADD = $(DAEMON_TEST_LIBS)

tests_daemon_test_boot_id_provider_SOURCES = \
	daemon/emer-boot-id-provider.c daemon/emer-boot-id-provider.h \
	tests/daemon/test-boot-id-provider.c \
	$(NULL)
tests_daemon_test_boot_id_provider_CPPFLAGS = $(DAEMON_TEST_FLAGS)
tests_daemon_test_boot_id_provider_LDADD = $(DAEMON_TEST_LIBS)

tests_daemon_test_daemon_dbusdaemon_SOURCES = \
	daemon/emer-daemon.c daemon/emer-daemon.h \
	tests/daemon/test-daemon.c \
	daemon/emer-boot-id-provider.c daemon/emer-boot-id-provider.h \
	daemon/emer-cache-version-provider.c daemon/emer-cache-version-provider.h \
	daemon/emer-machine-id-provider.c daemon/emer-machine-id-provider.h \
	tests/daemon/mock-network-send-provider.c \
	daemon/emer-network-send-provider.h \
	tests/daemon/mock-permissions-provider.c \
	tests/daemon/mock-permissions-provider.h \
	daemon/emer-permissions-provider.h \
	tests/daemon/mock-persistent-cache.c tests/daemon/mock-persistent-cache.h \
	shared/metrics-util.c shared/metrics-util.h \
	$(NULL)
tests_daemon_test_daemon_dbusdaemon_CPPFLAGS = \
	$(DAEMON_TEST_FLAGS) \
	-D_POSIX_C_SOURCE=200112L \
	$(NULL)
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
	daemon/emer-cache-version-provider.c daemon/emer-cache-version-provider.h \
	shared/metrics-util.c shared/metrics-util.h \
	$(NULL)
tests_daemon_test_persistent_cache_CPPFLAGS = \
	$(DAEMON_TEST_FLAGS) \
	-D_POSIX_C_SOURCE=200112L \
	$(NULL)
tests_daemon_test_persistent_cache_LDADD = $(DAEMON_TEST_LIBS)

javascript_tests = \
	$(NULL)
EXTRA_DIST += $(javascript_tests)

dist_noinst_SCRIPTS = \
	tests/launch-mock-event-recorder-tests.sh \
	tests/launch-mock-dbus-tests.sh \
	$(NULL)

# Run tests when running 'make check'
TESTS = \
	tests/test-library.dbuseventrecorder \
	tests/test-daemon-integration.py \
	tests/test-opt-out-integration.py \
	tests/test-event-types \
	tests/daemon/test-boot-id-provider \
	tests/daemon/test-cache-version-provider \
	tests/daemon/test-daemon.dbusdaemon \
	tests/daemon/test-machine-id-provider \
	tests/daemon/test-network-send-provider \
	tests/daemon/test-persistent-cache \
	tests/daemon/test-permissions-provider \
	$(javascript_tests) \
	$(NULL)

TEST_EXTENSIONS = .dbuseventrecorder .dbusdaemon .js .py
JS_LOG_COMPILER = eos-run-test
AM_JS_LOG_FLAGS = \
	--include-path=$(top_srcdir) \
	$(NULL)
DBUSEVENTRECORDER_LOG_COMPILER = $(top_srcdir)/tests/launch-mock-event-recorder-tests.sh
AM_DBUSEVENTRECORDER_LOG_FLAGS = -k --verbose
DBUSDAEMON_LOG_COMPILER = $(top_srcdir)/tests/launch-mock-dbus-tests.sh
AM_DBUSDAEMON_LOG_FLAGS = -k --verbose
LOG_COMPILER = gtester
AM_LOG_FLAGS = -k --verbose
PY_LOG_COMPILER = python

# Use locally built versions of EosMetrics-0.gir and libraries; this may need to
# be changed to AM_TESTS_ENVIRONMENT in a future version of Automake
# TODO: Consider an alternative to _MOCK_ENDLESSOS_VERSION_FILE since its use
# implies a need to set an environment variable by this name when running
# run-tests directly (as opposed to via make check).
TESTS_ENVIRONMENT = \
	export GI_TYPELIB_PATH="$(top_builddir)$${GI_TYPELIB_PATH:+:$$GI_TYPELIB_PATH}"; \
	export LD_LIBRARY_PATH="$(top_builddir)/.libs$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}"; \
	export _MOCK_ENDLESSOS_VERSION_FILE=`mktemp`; \
	$(NULL)

EXTRA_DIST += \
	tests/test-daemon-integration.py \
	tests/test-opt-out-integration.py \
	tests/smoke-tests/event-types.js \
	tests/smoke-tests/smokeEventRecorderHeavyPayload.js \
	tests/smoke-tests/smokeEventRecorder.js \
	tests/smoke-tests/smokeLibrary.js \
	tests/smoke-tests/smokeOpenProgram.js \
	tests/smoke-tests/smokeProgram.js \
	tests/wait-for-service-helper.py \
	$(NULL)

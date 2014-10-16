# Copyright 2014 Endless Mobile, Inc.

libexec_PROGRAMS = eos-metrics-event-recorder

daemon_dbus_name = emer-event-recorder-server

daemon_dbus_sources = \
	$(daemon_dbus_name).h \
	$(daemon_dbus_name).c \
	$(NULL)

BUILT_SOURCES += $(daemon_dbus_sources)
$(daemon_dbus_sources): $(EVENT_RECORDER_SERVER_XML)
	$(AM_V_GEN)$(GDBUS_CODEGEN) --generate-c-code $(daemon_dbus_name) \
		--c-namespace Emer \
		--interface-prefix com.endlessm.Metrics. \
		$<

CLEANFILES += $(daemon_dbus_sources)

eos_metrics_event_recorder_SOURCES = \
	daemon/emer-main.c \
	daemon/emer-boot-id-provider.c daemon/emer-boot-id-provider.h \
	daemon/emer-cache-version-provider.c daemon/emer-cache-version-provider.h \
	daemon/emer-daemon.c daemon/emer-daemon.h \
	daemon/emer-machine-id-provider.c daemon/emer-machine-id-provider.h \
	daemon/emer-network-send-provider.c daemon/emer-network-send-provider.h \
	daemon/emer-permissions-provider.c daemon/emer-permissions-provider.h \
	daemon/emer-persistent-cache.c daemon/emer-persistent-cache.h \
	emer-event-recorder-server.c emer-event-recorder-server.h \
	shared/metrics-util.c shared/metrics-util.h \
	$(NULL)

eos_metrics_event_recorder_CPPFLAGS = \
	$(EOS_EVENT_RECORDER_DAEMON_CFLAGS) \
	-D_POSIX_C_SOURCE=200112L \
	-DPERSISTENT_CACHE_DIR="\"$(persistentcachedir)\"" \
	-DPERMISSIONS_FILE="\"$(permissions_file)\"" \
	$(NULL)

eos_metrics_event_recorder_LDADD = $(EOS_EVENT_RECORDER_DAEMON_LIBS)

## Copyright 2014, 2015 Endless Mobile, Inc.

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
	daemon/emer-cache-size-provider.c daemon/emer-cache-size-provider.h \
	daemon/emer-cache-version-provider.c daemon/emer-cache-version-provider.h \
	daemon/emer-circular-file.c daemon/emer-circular-file.h \
	daemon/emer-daemon.c daemon/emer-daemon.h \
	daemon/emer-gzip.c daemon/emer-gzip.h \
	daemon/emer-machine-id-provider.c daemon/emer-machine-id-provider.h \
	daemon/emer-network-send-provider.c daemon/emer-network-send-provider.h \
	daemon/emer-permissions-provider.c daemon/emer-permissions-provider.h \
	daemon/emer-persistent-cache.c daemon/emer-persistent-cache.h \
	daemon/emer-types.c daemon/emer-types.h \
	emer-event-recorder-server.c emer-event-recorder-server.h \
	shared/metrics-util.c shared/metrics-util.h \
	$(NULL)

eos_metrics_event_recorder_CPPFLAGS = \
	$(EOS_EVENT_RECORDER_DAEMON_CFLAGS) \
	-DCONFIG_DIR="\"$(configdir)\"" \
	-DPERMISSIONS_FILE="\"$(permissions_file)\"" \
	-DPERSISTENT_CACHE_DIR="\"$(persistentcachedir)\"" \
	$(NULL)

eos_metrics_event_recorder_LDADD = $(EOS_EVENT_RECORDER_DAEMON_LIBS)

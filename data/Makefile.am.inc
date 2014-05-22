# Copyright 2014 Endless Mobile, Inc.

daemon_dbus_name = emer-event-recorder-server
daemon_dbus_sources = $(daemon_dbus_name).c $(daemon_dbus_name).h
BUILT_SOURCES += $(daemon_dbus_sources)
$(daemon_dbus_sources): data/com.endlessm.Metrics.xml
	$(AM_V_GEN)$(GDBUS_CODEGEN) --generate-c-code $(daemon_dbus_name) \
		--c-namespace Emer \
		--interface-prefix com.endlessm.Metrics. \
		$<

# Defined as systemdsystemunitdir in ../configure.ac.
systemdsystemunit_DATA = data/eos-metrics-event-recorder.service

data/eos-metrics-event-recorder.service: data/eos-metrics-event-recorder.service.in
	$(AM_V_GEN)mkdir -p data && \
	rm -f $@ $@.tmp && \
	$(edit) $< >$@.tmp && \
	mv $@.tmp $@

edit = sed \
	-e 's|@libexecdir[@]|$(libexecdir)|g' \
	$(NULL)

systembussecuritypolicydir = ${sysconfdir}/dbus-1/system.d
dist_systembussecuritypolicy_DATA = data/com.endlessm.Metrics.conf

EXTRA_DIST += \
	data/com.endlessm.Metrics.xml \
	data/eos-metrics-event-recorder.service.in \
	$(NULL)

CLEANFILES += \
	$(daemon_dbus_sources) \
	data/eos-metrics-event-recorder.service \
	$(NULL)

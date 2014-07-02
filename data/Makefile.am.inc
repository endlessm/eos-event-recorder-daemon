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

substitute_libexecdir = sed \
	-e 's|@libexecdir[@]|$(libexecdir)|g' \
	$(NULL)

data/eos-metrics-event-recorder.service: data/eos-metrics-event-recorder.service.in
	$(AM_V_GEN)$(MKDIR_P) data && \
	rm -f $@ $@.tmp && \
	$(substitute_libexecdir) $< >$@.tmp && \
	mv $@.tmp $@

dbusservicedir = ${datadir}/dbus-1/system-services
dbusservice_DATA = data/com.endlessm.Metrics.service

data/com.endlessm.Metrics.service: data/com.endlessm.Metrics.service.in
	$(AM_V_GEN)$(MKDIR_P) data && \
	rm -f $@ $@.tmp && \
	$(substitute_libexecdir) $< >$@.tmp && \
	mv $@.tmp $@

substitute_persistent_cache_dir = sed \
	-e 's|@PERSISTENT_CACHE_DIR[@]|$(PERSISTENT_CACHE_DIR)|g' \
	$(NULL)

data/eos-metrics-persistent-cache.conf: data/eos-metrics-persistent-cache.conf.in
	$(AM_V_GEN)$(MKDIR_P) data && \
	rm -f $@ $@.tmp && \
	$(substitute_persistent_cache_dir) $< >$@.tmp && \
	mv $@.tmp $@

policydir = ${datadir}/polkit-1/actions
dist_policy_DATA = data/com.endlessm.Metrics.policy

systembussecuritypolicydir = ${sysconfdir}/dbus-1/system.d
dist_systembussecuritypolicy_DATA = data/com.endlessm.Metrics.conf

dist_sysconf_DATA = data/eos-metrics-permissions.conf

tmpfilesddir = $(libdir)/tmpfiles.d/
tmpfilesd_DATA = data/eos-metrics-persistent-cache.conf

EXTRA_DIST += \
	data/com.endlessm.Metrics.xml \
	data/com.endlessm.Metrics.service.in \
	data/eos-metrics-event-recorder.service.in \
	data/eos-metrics-persistent-cache.conf.in \
	$(NULL)

CLEANFILES += \
	$(daemon_dbus_sources) \
	data/com.endlessm.Metrics.service \
	data/eos-metrics-event-recorder.service \
	data/eos-metrics-persistent-cache.conf \
	$(NULL)

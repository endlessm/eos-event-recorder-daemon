# Copyright 2014 Endless Mobile, Inc.

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

substitute_metrics_files = sed \
	-e 's|@persistentcachedir[@]|$(persistentcachedir)|g' \
	-e 's|@permissionsdir[@]|$(permissionsdir)|g' \
	$(NULL)

data/eos-metrics.conf: data/eos-metrics.conf.in
	$(AM_V_GEN)$(MKDIR_P) data && \
	rm -f $@ $@.tmp && \
	$(substitute_metrics_files) $< >$@.tmp && \
	mv $@.tmp $@

policydir = ${datadir}/polkit-1/actions
dist_policy_DATA = data/com.endlessm.Metrics.policy

systembussecuritypolicydir = ${sysconfdir}/dbus-1/system.d
dist_systembussecuritypolicy_DATA = data/com.endlessm.Metrics.conf

dist_permissions_DATA = data/eos-metrics-permissions.conf

tmpfilesddir = $(libdir)/tmpfiles.d/
tmpfilesd_DATA = data/eos-metrics.conf

EXTRA_DIST += \
	data/com.endlessm.Metrics.service.in \
	data/eos-metrics-event-recorder.service.in \
	data/eos-metrics.conf.in \
	$(NULL)

CLEANFILES += \
	data/com.endlessm.Metrics.service \
	data/eos-metrics-event-recorder.service \
	data/eos-metrics.conf \
	$(NULL)

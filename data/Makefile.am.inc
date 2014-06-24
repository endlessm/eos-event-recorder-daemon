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

dbusservicedir = ${datadir}/dbus-1/system-services
dbusservice_DATA = data/com.endlessm.Metrics.service

data/com.endlessm.Metrics.service: data/com.endlessm.Metrics.service.in
	$(AM_V_GEN)mkdir -p data && \
	rm -f $@ $@.tmp && \
	$(edit) $< >$@.tmp && \
	mv $@.tmp $@

systembussecuritypolicydir = ${sysconfdir}/dbus-1/system.d
dist_systembussecuritypolicy_DATA = data/com.endlessm.Metrics.conf

dist_sysconf_DATA = data/eos-metrics-permissions.conf

# chown and chgrp are not allowed unless you are root or under some specific
# other conditions that may not be met every time you run 'make'.
install-data-hook:
	@if id -u metrics 2>/dev/null; then \
		if ! chgrp metrics $(DESTDIR)$(sysconfdir)/eos-metrics-permissions.conf; then \
			echo "*************************************"; \
			echo "Make sure to change the group of the"; \
			echo "$(sysconfdir)/eos-metrics-permissions.conf file"; \
			echo "to 'metrics'."; \
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
	chmod 0664 $(DESTDIR)$(sysconfdir)/eos-metrics-permissions.conf

EXTRA_DIST += \
	data/com.endlessm.Metrics.xml \
	data/com.endlessm.Metrics.service.in \
	data/eos-metrics-event-recorder.service.in \
	$(NULL)

CLEANFILES += \
	$(daemon_dbus_sources) \
	data/com.endlessm.Metrics.service \
	data/eos-metrics-event-recorder.service \
	$(NULL)

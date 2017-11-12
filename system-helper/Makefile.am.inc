## Copyright 2017 Endless Mobile, Inc.

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

noinst_LTLIBRARIES += system-helper/libeos-metrics-system-helper.la
libexec_PROGRAMS += system-helper/eos-metrics-system-helper

system_helper_dbus_name = emer-metrics-system-helper

system_helper_sources = \
	system-helper/$(system_helper_dbus_name).h \
	system-helper/$(system_helper_dbus_name).c \
	$(NULL)

BUILT_SOURCES += $(system_helper_sources)
$(system_helper_sources): system-helper/com.endlessm.MetricsSystemHelper.xml
	$(AM_V_GEN)$(GDBUS_CODEGEN) --generate-c-code $(system_helper_dbus_name) \
		--c-namespace Emer \
		--interface-prefix com.endlessm \
		--output-directory=$(abs_top_builddir)/system-helper \
		$<

# Rules for building the dbus-activation service file
system-helper/com.endlessm.MetricsSystemHelper.service: system-helper/com.endlessm.MetricsSystemHelper.service.in
	$(AM_V_GEN)$(MKDIR_P) data && \
	rm -f $@ $@.tmp && \
	$(substitute_libexecdir) $< >$@.tmp && \
	mv $@.tmp $@

CLEANFILES += $(system_helper_sources)

# System helper d-bus library
nodist_system_helper_libeos_metrics_system_helper_la_SOURCES = \
	system-helper/emer-metrics-system-helper.c \
	system-helper/emer-metrics-system-helper.h \
	$(NULL)

system_helper_libeos_metrics_system_helper_la_CPPFLAGS = \
	$(EOS_METRICS_SYSTEM_HELPER_CFLAGS) \
	-DCONFIG_DIR="\"$(configdir)\"" \
	$(NULL)

system_helper_libeos_metrics_system_helper_la_LIBADD = \
	$(EOS_METRICS_SYSTEM_HELPER_LIBS) \
	$(top_builddir)/shared/libeos-metrics-event-recorder-shared.la \
	-lm

# System helper binary
system_helper_eos_metrics_system_helper_SOURCES = \
	system-helper/eos-event-recorder-system-helper.c \
	$(NULL)

system_helper_eos_metrics_system_helper_CPPFLAGS = \
	$(EOS_METRICS_SYSTEM_HELPER_CFLAGS) \
	-DCONFIG_DIR="\"$(configdir)\"" \
	-DSYSCONFDIR="\"$(sysconfdir)\"" \
	$(NULL)

system_helper_eos_metrics_system_helper_LDADD = \
	$(EOS_METRICS_SYSTEM_HELPER_LIBS) \
	$(top_builddir)/shared/libeos-metrics-event-recorder-shared.la \
	$(top_builddir)/system-helper/libeos-metrics-system-helper.la \
	$(NULL)

# DBus-activation service file
dist_dbusservice_DATA += system-helper/com.endlessm.MetricsSystemHelper.service

# DBus security policy
dist_systembussecuritypolicy_DATA += system-helper/com.endlessm.MetricsSystemHelper.conf

# Polkit policy
dist_policy_DATA += system-helper/com.endlessm.MetricsSystemHelper.policy

# Polkit rules
dist_policyrules_DATA += system-helper/20-eos-metrics.rules


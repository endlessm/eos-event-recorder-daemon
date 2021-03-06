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

systemdsystemunit_DATA += data/eos-metrics-event-recorder.service

data/eos-metrics-event-recorder.service: data/eos-metrics-event-recorder.service.in
	$(AM_V_GEN)$(MKDIR_P) data && \
	rm -f $@ $@.tmp && \
	$(substitute_libexecdir) $< >$@.tmp && \
	mv $@.tmp $@

dist_dbusservice_DATA += data/com.endlessm.Metrics.service

data/com.endlessm.Metrics.service: data/com.endlessm.Metrics.service.in
	$(AM_V_GEN)$(MKDIR_P) data && \
	rm -f $@ $@.tmp && \
	$(substitute_libexecdir) $< >$@.tmp && \
	mv $@.tmp $@

substitute_metrics_files = sed \
	-e 's|@persistentcachedir[@]|$(persistentcachedir)|g' \
	-e 's|@configdir[@]|$(configdir)|g' \
	$(NULL)

data/eos-metrics.conf: data/eos-metrics.conf.in
	$(AM_V_GEN)$(MKDIR_P) data && \
	rm -f $@ $@.tmp && \
	$(substitute_metrics_files) $< >$@.tmp && \
	mv $@.tmp $@

dist_policy_DATA += data/com.endlessm.Metrics.policy

dist_systembussecuritypolicy_DATA += data/com.endlessm.Metrics.conf

dist_config_DATA = \
	data/eos-metrics-permissions.conf \
	$(NULL)

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

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

noinst_LTLIBRARIES += shared/libeos-metrics-event-recorder-shared.la

nodist_shared_libeos_metrics_event_recorder_shared_la_SOURCES = \
	shared/metrics-util.c \
	shared/metrics-util.h \
	$(NULL)

shared_libeos_metrics_event_recorder_shared_la_CPPFLAGS = \
	$(EOS_EVENT_RECORDER_DAEMON_CFLAGS) \
	-DCONFIG_DIR="\"$(configdir)\"" \
	$(NULL)

shared_libeos_metrics_event_recorder_shared_la_LIBADD = $(EOS_EVENT_RECORDER_SHARED_LIBS)


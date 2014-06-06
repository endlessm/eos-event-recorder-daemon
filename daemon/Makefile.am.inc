# Copyright 2014 Endless Mobile, Inc.

libexec_PROGRAMS = eos-metrics-event-recorder

## emer-event-recorder-server.[ch] are generated in ../data/Makefile.am.inc.
eos_metrics_event_recorder_SOURCES = \
	daemon/emer-main.c \
	daemon/emer-daemon.c daemon/emer-daemon.h \
	daemon/emer-machine-id-provider.c daemon/emer-machine-id-provider.h \
	emer-event-recorder-server.c emer-event-recorder-server.h \
	shared/metrics-util.c shared/metrics-util.h \
	$(NULL)
eos_metrics_event_recorder_CPPFLAGS = $(EOSMETRICS_EVENT_RECORDER_CFLAGS)
eos_metrics_event_recorder_LDADD = $(EOSMETRICS_EVENT_RECORDER_LIBS)

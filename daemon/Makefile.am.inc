# Copyright 2014 Endless Mobile, Inc.

libexec_PROGRAMS = eos-metrics-event-recorder

## emer-event-recorder-server.[ch] are generated in ../data/Makefile.am.inc.
eos_metrics_event_recorder_SOURCES = \
	daemon/emer-main.c \
	emer-event-recorder-server.c emer-event-recorder-server.h \
	$(NULL)
eos_metrics_event_recorder_CPPFLAGS = $(EOSMETRICS_EVENT_RECORDER_CFLAGS)
eos_metrics_event_recorder_LDADD = $(EOSMETRICS_EVENT_RECORDER_LIBS)

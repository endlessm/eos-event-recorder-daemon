# Copyright 2013 Endless Mobile, Inc.

eosmetrics_public_installed_headers = eosmetrics/eosmetrics.h

eosmetrics_private_installed_headers = \
	eosmetrics/emtr-enums.h \
	eosmetrics/emtr-event-recorder.h \
	eosmetrics/emtr-event-types.h \
	eosmetrics/emtr-macros.h \
	eosmetrics/emtr-types.h \
	eosmetrics/emtr-version.h \
	$(NULL)

eosmetrics_library_sources = \
	emer-event-recorder-server.c emer-event-recorder-server.h \
	eosmetrics/emtr-event-recorder.c \
	shared/metrics-util.c shared/metrics-util.h \
	$(NULL)

lib_LTLIBRARIES = libeosmetrics-@EMTR_API_VERSION@.la
libeosmetrics_@EMTR_API_VERSION@_la_SOURCES = \
	$(eosmetrics_public_installed_headers) \
	$(eosmetrics_private_installed_headers) \
	$(eosmetrics_library_sources) \
	$(NULL)
# Defining G_LOG_DOMAIN allows you to run any program with
# G_MESSAGES_DEBUG=EosMetrics ./myprogram
# and turn on debug messages
libeosmetrics_@EMTR_API_VERSION@_la_CPPFLAGS = \
	@EOSMETRICS_CFLAGS@ \
	-DG_LOG_DOMAIN=\"EosMetrics\" \
	-DCOMPILING_EOS_METRICS \
	-DPERMISSIONS_FILE="\"$(permissions_file)\"" \
	-D_POSIX_C_SOURCE=200112L \
	$(NULL)
libeosmetrics_@EMTR_API_VERSION@_la_CFLAGS = $(AM_CFLAGS)
libeosmetrics_@EMTR_API_VERSION@_la_LIBADD = @EOSMETRICS_LIBS@
libeosmetrics_@EMTR_API_VERSION@_la_LDFLAGS = \
	-version-info @EMTR_LT_VERSION_INFO@ \
	-export-symbols-regex "^emtr_" \
	$(NULL)

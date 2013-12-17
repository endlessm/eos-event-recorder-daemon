# Copyright 2013 Endless Mobile, Inc.

eosmetrics_public_installed_headers = eosmetrics/eosmetrics.h

eosmetrics_private_installed_headers = \
	eosmetrics/emtr-enums.h \
	eosmetrics/emtr-macros.h \
	eosmetrics/emtr-types.h \
	eosmetrics/emtr-version.h \
	eosmetrics/emtr-connection.h \
	$(NULL)

eosmetrics_library_sources = \
	eosmetrics/emtr-connection.c \
	eosmetrics/emtr-osversion.c eosmetrics/emtr-osversion-private.h \
	eosmetrics/emtr-uuid.c eosmetrics/emtr-uuid-private.h \
	eosmetrics/emtr-web.c eosmetrics/emtr-web-private.h \
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
	-DDATADIR=\""$(datadir)"\" \
	-DSYSCONFDIR=\""$(sysconfdir)"\" \
	$(NULL)
libeosmetrics_@EMTR_API_VERSION@_la_CFLAGS = $(AM_CFLAGS)
libeosmetrics_@EMTR_API_VERSION@_la_LIBADD = @EOSMETRICS_LIBS@
libeosmetrics_@EMTR_API_VERSION@_la_LDFLAGS = \
	-version-info @EMTR_LT_VERSION_INFO@ \
	-export-symbols-regex "^emtr_" \
	$(NULL)

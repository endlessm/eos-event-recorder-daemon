# Copyright 2014 Endless Mobile, Inc.

noinst_PROGRAMS += \
	tools/print-persistent-cache \
	$(NULL)

PERSISTENT_CACHE_TOOL_FLAGS = \
	@EOSMETRICS_EVENT_RECORDER_CFLAGS@ \
	-I$(top_srcdir) \
	-I$(top_srcdir)/daemon \
	-I$(top_srcdir)/shared \
	-I$(top_srcdir)/tools \
	-DPERSISTENT_CACHE_DIR="\"$(persistentcachedir)\"" \
	-D_POSIX_C_SOURCE=200112L \
	$(NULL)
PERSISTENT_CACHE_TOOL_LIBS = @EOSMETRICS_EVENT_RECORDER_LIBS@

tools_print_persistent_cache_SOURCES = \
	daemon/emer-boot-id-provider.c daemon/emer-boot-id-provider.h \
	daemon/emer-cache-version-provider.c daemon/emer-cache-version-provider.h \
	daemon/emer-persistent-cache.c daemon/emer-persistent-cache.h \
	shared/metrics-util.c shared/metrics-util.h \
	tools/print-persistent-cache.c \
	$(NULL)
tools_print_persistent_cache_CPPFLAGS = $(PERSISTENT_CACHE_TOOL_FLAGS)
tools_print_persistent_cache_LDADD = $(PERSISTENT_CACHE_TOOL_LIBS)

dist_bin_SCRIPTS = tools/eos-select-metrics-env
EXTRA_DIST += tools/eos-select-metrics-env.in
CLEANFILES += tools/eos-select-metrics-env

substitute_permissions_file = sed \
	-e 's|@permissions_file[@]|$(permissions_file)|g' \
	$(NULL)
tools/eos-select-metrics-env: tools/eos-select-metrics-env.in Makefile
	$(AM_V_GEN)$(MKDIR_P) tools && \
	rm -f $@ $@.tmp && \
	$(substitute_permissions_file) $< >$@.tmp && \
	chmod +x,a-w $@.tmp && \
	mv $@.tmp $@

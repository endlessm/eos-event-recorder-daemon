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

noinst_PROGRAMS += \
	tools/print-persistent-cache \
	$(NULL)

PERSISTENT_CACHE_TOOL_FLAGS = \
	@EOS_EVENT_RECORDER_DAEMON_CFLAGS@ \
	-I$(top_srcdir) \
	-I$(top_srcdir)/daemon \
	-I$(top_srcdir)/shared \
	-I$(top_srcdir)/tools \
	-DCONFIG_DIR="\"$(configdir)\"" \
	-DPERMISSIONS_FILE="\"$(permissions_file)\"" \
	-DPERSISTENT_CACHE_DIR="\"$(persistentcachedir)\"" \
	$(NULL)
PERSISTENT_CACHE_TOOL_LIBS = @EOS_EVENT_RECORDER_DAEMON_LIBS@

tools_print_persistent_cache_SOURCES = \
	daemon/emer-boot-id-provider.c daemon/emer-boot-id-provider.h \
	daemon/emer-cache-size-provider.c daemon/emer-cache-size-provider.h \
	daemon/emer-cache-version-provider.c daemon/emer-cache-version-provider.h \
	daemon/emer-circular-file.c daemon/emer-circular-file.h \
	daemon/emer-persistent-cache.c daemon/emer-persistent-cache.h \
	shared/metrics-util.c shared/metrics-util.h \
	tools/print-persistent-cache.c \
	$(NULL)
tools_print_persistent_cache_CPPFLAGS = $(PERSISTENT_CACHE_TOOL_FLAGS)
tools_print_persistent_cache_LDADD = $(PERSISTENT_CACHE_TOOL_LIBS)

dist_bin_SCRIPTS = \
	tools/eos-enable-metrics-uploading \
	tools/eos-select-metrics-env \
	tools/eos-upload-metrics \
	$(NULL)
EXTRA_DIST += \
	tools/eos-enable-metrics-uploading.in \
	tools/eos-select-metrics-env.in \
	$(NULL)
CLEANFILES += \
	tools/eos-enable-metrics-uploading \
	tools/eos-select-metrics-env \
	$(NULL)

substitute_permissions_file = sed \
	-e 's|@permissions_file[@]|$(permissions_file)|g' \
	$(NULL)

tools/eos-enable-metrics-uploading: tools/eos-enable-metrics-uploading.in Makefile
	$(AM_V_GEN)$(MKDIR_P) tools && \
	rm -f $@ $@.tmp && \
	$(substitute_permissions_file) $< >$@.tmp && \
	chmod +x,a-w $@.tmp && \
	mv $@.tmp $@

tools/eos-select-metrics-env: tools/eos-select-metrics-env.in Makefile
	$(AM_V_GEN)$(MKDIR_P) tools && \
	rm -f $@ $@.tmp && \
	$(substitute_permissions_file) $< >$@.tmp && \
	chmod +x,a-w $@.tmp && \
	mv $@.tmp $@

# Copyright 2014 Endless Mobile, Inc.

dist_bin_SCRIPTS = tools/eos-select-metrics-env
EXTRA_DIST += tools/eos-select-metrics-env.in
CLEANFILES += tools/eos-select-metrics-env

substitute_etc = sed \
	-e 's|@sysconfdir[@]|$(sysconfdir)|g' \
	$(NULL)
tools/eos-select-metrics-env: tools/eos-select-metrics-env.in Makefile
	$(AM_V_GEN)$(MKDIR_P) tools && \
	rm -f $@ $@.tmp && \
	$(substitute_etc) $< >$@.tmp && \
	chmod +x,a-w $@.tmp && \
	mv $@.tmp $@

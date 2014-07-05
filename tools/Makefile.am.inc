# Copyright 2014 Endless Mobile, Inc.

dist_bin_SCRIPTS = tools/eos-set-metrics-env
EXTRA_DIST += tools/eos-set-metrics-env.in
CLEANFILES += tools/eos-set-metrics-env

substitute_etc = sed \
	-e 's|@sysconfdir[@]|$(sysconfdir)|g' \
	$(NULL)
tools/eos-set-metrics-env: tools/eos-set-metrics-env.in Makefile
	$(AM_V_GEN)$(MKDIR_P) tools && \
	rm -f $@ $@.tmp && \
	$(substitute_etc) $< >$@.tmp && \
	chmod +x,a-w $@.tmp && \
	mv $@.tmp $@

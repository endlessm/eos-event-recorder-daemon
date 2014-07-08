# Copyright 2014 Endless Mobile, Inc.

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

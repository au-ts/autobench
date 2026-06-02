#
# Copyright 2026, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause
#

EXPERIMENT_MAKEFILES := $(wildcard experiments/*/Makefile) \
                        $(wildcard experiments/*/*/Makefile)
EXPERIMENT_DIRS      := $(sort $(dir $(EXPERIMENT_MAKEFILES)))

.PHONY: all clean $(EXPERIMENT_DIRS)

all: $(EXPERIMENT_DIRS)

# Build each experiment, forwarding the toolchain so Buildroot's
# cross-compiler flags are respected by the sub-make.
$(EXPERIMENT_DIRS):
	@echo "ts_autobench->Building experiment: $@"
	$(MAKE) -C $@ \
		CC="$(CC)" \
		CXX="$(CXX)" \
		CFLAGS="$(CFLAGS)" \
		CXXFLAGS="$(CXXFLAGS)" \
		LDFLAGS="$(LDFLAGS)" \
		AR="$(AR)" \
		LD="$(LD)"

clean:
	# call recursive clean for each experiment
	@for dir in $(EXPERIMENT_DIRS); do \
		$(MAKE) -C $$dir clean || true; \
	done

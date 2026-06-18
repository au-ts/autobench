#
# Copyright 2026, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause
#

TS_AUTOBENCH_SITE = git@github.com:au-ts/autobench.git
TS_AUTOBENCH_SITE_METHOD = git

# note: this MUST match config.in
TS_AUTOBENCH_DEPENDENCIES = python3 python-lxml iperf3 iperf
# gather target platform cross-compile toolchain
define TS_AUTOBENCH_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		CXX="$(TARGET_CXX)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		CXXFLAGS="$(TARGET_CXXFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		AR="$(TARGET_AR)" \
		LD="$(TARGET_LD)"
endef

# we don't install this like a normal package ... we instead want to just
# build everything and then inject it to the rootfs.
define TS_AUTOBENCH_INSTALL_TARGET_CMDS
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/share/ts_autobench

	# Copy the complete source tree into the root filesystem.
	# We use tar with -h (--dereference) so symlinked dirs in the experiments
	# folder (e.g. the src/ipbench_old symlink) are expanded into real files;
	# otherwise it would be a broken absolute
	# symlink on the target.  The .git directory is excluded for obvious reasons
	tar -C $(@D) -h --exclude='.git' -cf - . | \
		tar -C $(TARGET_DIR)/ts_autobench -xf -

	# Ensure controlplane scripts and experiment runners are executable.
	find $(TARGET_DIR)/usr/share/ts_autobench -type f \
		\( -name "*.sh" -o -name "*.py" \) \
		-exec chmod 0755 {} +
endef

# init script: just do basics we always want for benchmarks
define TS_AUTOBENCH_GENERATE_INIT_SCRIPT
	#!/bin/sh
	# ts_autobench startup script - runs at boot as S99ts_autobench

	# 1. Run udhcpc and wait for it to complete
	udhcpc -n -i
	# -n flag makes udhcpc exit immediately if it fails to obtain a lease

	# print ip addr
	ip addr

	# 3. Run ipbenchd
	ipbenchd --target &
endef

$(eval $(generic-package))

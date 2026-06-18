#
# Copyright 2026, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause
#

TS_AUTOBENCH_VERSION = main
TS_AUTOBENCH_SITE = https://github.com/au-ts/autobench/archive/refs/heads
TS_AUTOBENCH_SOURCE = $(TS_AUTOBENCH_VERSION).tar.gz
TS_AUTOBENCH_SITE_METHOD = wget

# note: this MUST match Config.in
TS_AUTOBENCH_DEPENDENCIES = python3 python-lxml iperf3 iperf

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

# Copy the complete source tree (with compiled binaries) into the rootfs.
define TS_AUTOBENCH_INSTALL_TARGET_CMDS
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/share/ts_autobench

	tar -C $(@D) -h --exclude='.git' -cf - . | \
		tar -C $(TARGET_DIR)/usr/share/ts_autobench -xf -

	find $(TARGET_DIR)/usr/share/ts_autobench -type f \
		\( -name "*.sh" -o -name "*.py" \) \
		-exec chmod 0755 {} +
endef

$(eval $(generic-package))


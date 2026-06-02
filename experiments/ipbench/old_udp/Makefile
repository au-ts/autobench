#
# Copyright 2026, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause
#

BUILD_DIR = build
CFLAGS = -Wall -Werror -O3
LDFLAGS += -lpthread

all: echoit

%.c: | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean::
	rm echoit

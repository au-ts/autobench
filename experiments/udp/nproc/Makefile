#
# Copyright 2026, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause
#

BUILD_DIR = build
CFLAGS = -Wall -Werror -O3
LDFLAGS += -lpthread

all: echoitnproc

%.c: | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	cp $@ $(BUILD_DIR)/../

clean::
	rm echoit2

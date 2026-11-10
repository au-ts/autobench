#
# Copyright 2026, UNSW
#
# SPDX-License-Identifier: BSD-2-Clause
#

BUILD_DIR = build
CFLAGS = -Wall -Werror -O3
LDLIBS += -lpthread -luring

all: io_uring_echo

io_uring_echo: io_uring_echo.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean::
	rm -f io_uring_echo

# Copyright (c) 2026 Daniel
# SPDX-License-Identifier: BSD-3-Clause

CC     = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
         -fno-strict-aliasing -D_GNU_SOURCE -fPIC -shared
SEA_SRC = ../../src

.PHONY: all clean

all: compress.so

compress.so: compress.c $(wildcard ../../src/*.h)
	$(CC) $(CFLAGS) -I$(SEA_SRC) -o $@ $< -lz

clean:
	rm -f compress.so

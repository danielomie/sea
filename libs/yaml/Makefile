# Copyright (c) 2026 Daniel
# SPDX-License-Identifier: BSD-3-Clause

CC     = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
         -fno-strict-aliasing -D_GNU_SOURCE -fPIC -shared
SEA_SRC = ../../src

.PHONY: all clean

all: yaml.so

yaml.so: yaml.c $(wildcard ../../src/*.h)
	$(CC) $(CFLAGS) -I$(SEA_SRC) -o $@ $< -lm

clean:
	rm -f yaml.so

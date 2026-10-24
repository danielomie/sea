# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause

CC     = gcc
CFLAGS = -std=c11 -O3 -march=native -fno-math-errno -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
         -fno-strict-aliasing -D_GNU_SOURCE -fPIC -shared
SEA_SRC = ../../src

.PHONY: all clean test

all: ndarray_native.so

ndarray_native.so: ndarray_native.c $(wildcard ../../src/*.h)
	$(CC) $(CFLAGS) -I$(SEA_SRC) -o $@ $< -lm -lpthread

test: ndarray_native.so
	../../bin/sea test .

clean:
	rm -f ndarray_native.so

# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause

CC     = gcc
CFLAGS = -std=c11 -O3 -march=native -mtune=native -Wall -Wextra -Wno-unused-parameter \
         -fno-strict-aliasing -D_GNU_SOURCE
LDFLAGS = -lpthread -lm -ldl -rdynamic

# The REPL uses readline for line editing when its headers are installed
# (src/sea_main.c checks with __has_include); link it in that case.
HAVE_READLINE := $(shell printf '\043include <stdio.h>\n\043include <readline/readline.h>\n' | \
                   $(CC) -E -x c - >/dev/null 2>&1 && echo 1)
ifeq ($(HAVE_READLINE),1)
LDFLAGS += -lreadline
endif

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin

SRCS    = $(wildcard $(SRC_DIR)/*.c)
OBJS    = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
TARGET  = $(BIN_DIR)/sea

# Separate JIT-enabled binary (coexists with the plain interpreter bin/sea),
# built into its own object dir so the two never clobber each other.
JIT_OBJ_DIR = obj-jit
JIT_OBJS    = $(SRCS:$(SRC_DIR)/%.c=$(JIT_OBJ_DIR)/%.o)
JIT_TARGET  = $(BIN_DIR)/sea-jit

.PHONY: all clean test test-jit test-seac test-build test-dwe test-format test-license asan debug noreadline avx2 lib jemalloc test-jemalloc jit sea-jit

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJS:.o=.d)

# Standalone JIT binary at bin/sea-jit (separate objects under obj-jit/).
sea-jit: $(JIT_TARGET)

$(JIT_TARGET): $(JIT_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -DSEA_JIT -o $@ $^ $(LDFLAGS)

$(JIT_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(JIT_OBJ_DIR)
	$(CC) $(CFLAGS) -DSEA_JIT -MMD -MP -c -o $@ $<

-include $(JIT_OBJS:.o=.d)

# The `sea fmt`, `sea doc`, ... tools: Sea scripts in tools/, embedded in the
# binary as byte arrays (gen/sea_tools.h, included by sea_main.c). Modules
# that `require("sea/...")` finds without a file: gen/sea_modules.h, included
# by sea_stdlib.c (name=file).
TOOLS = $(wildcard tools/*.sea)
MODULES = sea/test=libs/test/test.sea sea/check=tools/lib/check.sea
GEN_DIR = gen
CFLAGS += -I$(GEN_DIR)

$(GEN_DIR)/sea_tools.h: $(TOOLS) tools/embed.sh | $(GEN_DIR)
	sh tools/embed.sh SEA_TOOLS $(TOOLS) > $@

$(GEN_DIR)/sea_modules.h: $(foreach m,$(MODULES),$(lastword $(subst =, ,$(m)))) tools/embed.sh | $(GEN_DIR)
	sh tools/embed.sh SEA_MODULES $(MODULES) > $@

$(OBJ_DIR)/sea_main.o $(JIT_OBJ_DIR)/sea_main.o: $(GEN_DIR)/sea_tools.h
$(OBJ_DIR)/sea_stdlib.o $(JIT_OBJ_DIR)/sea_stdlib.o: $(GEN_DIR)/sea_modules.h

$(BIN_DIR) $(OBJ_DIR) $(JIT_OBJ_DIR) $(GEN_DIR):
	mkdir -p $@

avx2: CFLAGS += -mavx2
avx2: $(TARGET)

# Build with jemalloc allocator (sudo apt install libjemalloc-dev)
# sea_free() passes size → je_sdallocx skips the internal size-class lookup
jemalloc: CFLAGS  += -DUSE_JEMALLOC
jemalloc: LDFLAGS += -ljemalloc
jemalloc: $(TARGET)

test-jemalloc: jemalloc
	@$(TARGET) tests/run_tests.sea

# Dead-write elimination checks: with SEA_DWE_CHECK=1 an eliminated write loads
# a poison string instead of becoming a NOP (a write wrongly judged dead then
# shows up in the output); SEA_NO_DWE=1 compiles with no elimination at all.
# SEA_TYPE_CHECK=1 makes IIADD/IISUB (emitted only for operands the compiler
# proved int) abort if that proof is wrong.
test-dwe: $(TARGET)
	SEA_DWE_CHECK=1 SEA_TYPE_CHECK=1 bash tests/run_tests.sh
	SEA_NO_DWE=1 bash tests/run_tests.sh

# Unit tests run as compiled bytecode: each test is `sea compile`d to .seac and
# that file is executed (catches serialization bugs in sea_chunk_write/read).
test-seac: $(TARGET)
	SEAC=1 bash tests/run_tests.sh

# Unit tests run as standalone executables made by `sea build`
test-build: $(TARGET)
	BUNDLE=1 bash tests/run_tests.sh

# Unit tests on bin/sea-jit with every chunk JIT-compiled at its first call /
# loop back-edge, synchronously (deterministic), then asynchronously, then with
# the default thresholds (code compiled after the interpreter has recorded type
# feedback, which selects the typed-array/table fallbacks). STRICT
# makes any JIT codegen/compile failure abort instead of silently interpreting.
test-jit: $(JIT_TARGET)
	SEA=$(JIT_TARGET) SEA_FLAGS=--jit SEA_JIT_STRICT=1 SEA_JIT_SYNC=1 SEA_JIT_HOT_CALLS=1 SEA_JIT_HOT_LOOPS=1 bash tests/run_tests.sh
	SEA=$(JIT_TARGET) SEA_FLAGS=--jit SEA_JIT_STRICT=1 SEA_JIT_HOT_CALLS=1 SEA_JIT_HOT_LOOPS=1 bash tests/run_tests.sh
	SEA=$(JIT_TARGET) SEA_FLAGS=--jit SEA_JIT_STRICT=1 SEA_JIT_SYNC=1 bash tests/run_tests.sh

asan: CFLAGS += -fsanitize=address,undefined -g -O1
asan: $(TARGET)

# Build with the optional method-JIT enabled (see docs/feature_jit.md).
# Needs libgccjit: `sudo apt install libgccjit-dev` (headers at build time; the
# library itself is dlopen()ed at run time, only when --jit is used).
# Object files are shared with the default build, so run `make clean` first when
# switching between a plain build and a JIT build.
jit: CFLAGS  += -DSEA_JIT
jit: $(TARGET)

debug: CFLAGS += -g -O0 -DSEA_DEBUG
debug: $(TARGET)

# Build without readline (for environments without it)
noreadline: CFLAGS += -DSEA_NO_READLINE
noreadline: LDFLAGS = -lpthread -lm -ldl -rdynamic
noreadline: $(TARGET)

# Build an external Sea library: make lib SRC=mylib.c OUT=mylib.so
# Example: make lib SRC=examples/hello.c OUT=examples/hello.so
lib:
ifndef SRC
	$(error Usage: make lib SRC=mylib.c OUT=mylib.so)
endif
	$(CC) -shared -fPIC $(CFLAGS) \
	    -I$(SRC_DIR) \
	    -o $(if $(OUT),$(OUT),$(basename $(SRC)).so) \
	    $(SRC)

test: all
	@$(TARGET) tests/run_tests.sea
	@$(TARGET) util/license.sea -l BSD-3-Clause -c "The Sea Authors" -check

# every source file has the BSD-3-Clause header (fix with: bin/sea util/license.sea -l BSD-3-Clause -c "The Sea Authors")
test-license: $(TARGET)
	@$(TARGET) util/license.sea -l BSD-3-Clause -c "The Sea Authors" -check

# sea/parser's format round trip over every .sea file (see the script)
test-format: $(TARGET)
	@$(TARGET) tests/format_roundtrip.sea $$(find . -name '*.sea' -not -path './.git/*' | sort)

# Run tests under AddressSanitizer (leak detection disabled; register-borrowing
# semantics mean local variables that are never stored to a container will appear
# as leaks at exit. Use-after-free and other errors are still detected.)
test-asan: clean
	$(MAKE) asan
	@ASAN_OPTIONS=detect_leaks=0 $(TARGET) tests/run_tests.sea

clean:
	rm -rf $(OBJ_DIR) $(JIT_OBJ_DIR) $(BIN_DIR) $(GEN_DIR)

/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: MIT
 */

#pragma once

typedef struct SeaVM SeaVM;
typedef struct SeaChunk SeaChunk;

/*
 * Line coverage. With SEA_COVER=<file> in the environment, the compiler emits
 * an OP_COVER at each statement (sea_cover_enabled) and the run appends, at
 * its end or at exit(), an lcov record per source file to <file>:
 *   SF:<path>  DA:<line>,<0|1> ...  LF:<lines>  LH:<lines run>  end_of_record
 * `sea test -cover` sets it for each test file and merges the records.
 */

void sea_cover_start(const char *out_path);
void sea_cover_set_root(SeaVM *vm, SeaChunk *root);
void sea_cover_finish(void);

/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include <stdbool.h>

typedef struct SeaVM SeaVM;
typedef struct SeaChunk SeaChunk;

/*
 * `sea prof`: a sampling CPU profiler. Each thread that runs Sea code (the
 * main thread and the scheduler's workers) gets a CPU-time timer; its SIGPROF
 * handler copies the call stack of the VM running on that thread
 * (sea_gc_vm) into a preallocated buffer: (closure's chunk, ip) per frame.
 * After the run (or at exit()) the samples are resolved to functions and
 * lines and reported.
 */

typedef struct {
    int         hz;        /* samples per second of CPU time (default 100) */
    const char *out;       /* report file (NULL: stderr) */
    bool        folded;    /* folded stacks (flame graphs) instead of the text report */
    int         top;       /* rows in each table of the text report (default 20) */
} SeaProfOptions;

extern bool sea_prof_enabled;

void sea_prof_start(const SeaProfOptions *opts);
/* A thread that will run Sea code starts its own timer (workers call it). */
void sea_prof_thread_start(void);
/* The roots whose chunks the samples point into (main script; modules via vm). */
void sea_prof_set_root(SeaVM *vm, SeaChunk *root);
/* Stop sampling and write the report (once; also runs at exit()). */
void sea_prof_finish(void);

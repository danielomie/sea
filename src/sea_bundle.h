/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>

/*
 * Programs bundled by `sea build`: a copy of the sea binary with the
 * program's modules appended. Layout of the file:
 *
 *   <the sea binary> <payload> <trailer>
 *   payload: entries "<kind> <size> <name>\n" + <size> bytes, then "E\n";
 *            kind S is Sea source, N a native library (.so); the first
 *            entry is the main script; names are paths relative to the
 *            bundle's root (app.sea, libs/json/json.so)
 *   trailer: the payload's offset as 20 decimal digits, then "SEABNDL1"
 *
 * At startup main() calls sea_bundle_open(): when the binary carries a
 * payload, it runs the main entry and require() looks in the bundle first.
 */

typedef struct {
    char        kind;      /* 'S' source (NUL-terminated), 'N' native */
    const char *name;
    const char *data;
    size_t      size;
} SeaBundleEntry;

#define SEA_BUNDLE_MAGIC "SEABNDL1"

/* Load the payload of this executable, if it has one. */
bool sea_bundle_open(void);

/* The main script, or NULL when this binary is not a bundle. */
const SeaBundleEntry *sea_bundle_main(void);

/* require(name) from a module whose source name is from: the entry, or NULL.
 * Tries from's directory + name (.sea, /init.sea, .so), then any entry whose
 * path ends with /name.sea or /name.so (a module found through the working
 * directory or SEA_PATH when the bundle was built). */
const SeaBundleEntry *sea_bundle_find(const char *from, const char *name);

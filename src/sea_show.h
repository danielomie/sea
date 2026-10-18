/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "sea_value.h"
#include "sea_string.h"

struct SeaVM;

/* Nesting shown by print / tostring before [...] / {...} */
#define SEA_SHOW_DEPTH 4

/* The one-line text of a value: containers with their contents ([1, "a"],
 * {k = 1}, [="k" = 1]), strings inside them quoted. New string (RC 1). */
SeaString *sea_show(struct SeaVM *vm, SeaValue v, int max_depth);

/* The same, indented over several lines where a container does not fit in
 * 80 columns (dump). Strings are quoted. New string (RC 1). */
SeaString *sea_dump(struct SeaVM *vm, SeaValue v);

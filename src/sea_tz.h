/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A named time zone ("America/Sao_Paulo") read from the system's tz
 * database (TZif files under $TZDIR or /usr/share/zoneinfo). Loaded zones
 * are kept for the life of the process and shared by all threads. */
typedef struct SeaTZ SeaTZ;

/* The zone with that IANA name, or NULL with a message in err */
const SeaTZ *sea_tz_load(const char *name, char *err, size_t errlen);

/* The zone's name, as given to sea_tz_load */
const char *sea_tz_name(const SeaTZ *tz);

/* The offset east of UTC (seconds) in effect at the instant t (seconds
 * since the epoch), with its abbreviation ("-03", "EST") when abbr is not NULL */
int64_t sea_tz_offset(const SeaTZ *tz, int64_t t, const char **abbr);

/* The instant whose wall-clock time in tz is `wall` (wall: seconds since
 * the epoch as if the zone were UTC). A time skipped by a change to summer
 * time is moved forward by the gap; a repeated time takes the first one
 * (as Go does). */
int64_t sea_tz_from_wall(const SeaTZ *tz, int64_t wall);

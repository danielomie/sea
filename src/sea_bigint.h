/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "sea_object.h"
#include "sea_string.h"

/*
 * Arbitrary-precision integers (type "bigint").
 *
 * An explicit type: values come from bigint.new(), from the bigint module's
 * arithmetic, or from libraries that read integers wider than Sea's 48-bit
 * int (libs/duckdb, json.decode with bigint = true). The arithmetic
 * operators do not take bigints (the compiler treats + - * / as producing
 * scalars); comparison operators, ==, tostring and the natural sort order
 * do, mixing freely with int and number.
 *
 * Immutable: magnitude in 32-bit limbs, least significant first, no leading
 * zero limbs (n == 0 is zero, never negative).
 */
typedef struct {
    SeaObject obj;
    uint32_t  n;        /* limbs in use */
    uint32_t  cap;      /* limbs allocated */
    uint32_t  neg;      /* 1: negative (never for zero) */
    uint32_t  d[];
} SeaBigInt;

#define sea_is_bigint(v)  sea_obj_is_type(v, OBJ_BIGINT)
#define SEA_AS_BIGINT(v)  ((SeaBigInt *)sea_as_obj(v))

/* Constructors: a new bigint (rc = 1). */
SeaBigInt *sea_bigint_from_i64(int64_t v);
SeaBigInt *sea_bigint_from_u64(uint64_t v);
/* d must be finite; the fraction is dropped (toward zero). */
SeaBigInt *sea_bigint_from_double(double d);
/* Magnitude as len big-endian bytes, plus a sign. */
SeaBigInt *sea_bigint_from_bytes(const uint8_t *be, size_t len, bool neg);
/* Digits in base 2..36 (base 0: decimal, or 0x / 0o / 0b prefix), optional
 * sign, '_' between digits allowed. NULL if s is not such a number. */
SeaBigInt *sea_bigint_parse(const char *s, size_t len, int base);

/* Magnitude bytes needed (0 for zero), and the magnitude big-endian into
 * out[0..len) (len >= sea_bigint_byte_len, zero-padded on the left). */
size_t     sea_bigint_byte_len(const SeaBigInt *b);
void       sea_bigint_to_bytes(const SeaBigInt *b, uint8_t *out, size_t len);

bool       sea_bigint_to_i64(const SeaBigInt *b, int64_t *out);     /* false if it does not fit */
double     sea_bigint_to_double(const SeaBigInt *b);                 /* nearest */
SeaString *sea_bigint_to_string(const SeaBigInt *b, int base);       /* base 2..36, rc = 1 */

int        sea_bigint_cmp(const SeaBigInt *a, const SeaBigInt *b);
uint32_t   sea_bigint_hash(const SeaBigInt *b);
size_t     sea_bigint_size(const SeaBigInt *b);                      /* allocation size */

/* Ordering of two numeric values (bigint, int or number), at least one a
 * bigint: 1 with *out = -1/0/1; 0 when unordered (a NaN: every comparison
 * is false); -1 when a value is not numeric (an error). */
int        sea_bigint_compare(SeaValue a, SeaValue b, int *out);

/* Arithmetic on bigint, int or integral number operands; results are new
 * bigints. NULL with *err set: "integer expected", "division by zero",
 * "result too large". */
SeaBigInt *sea_bigint_arith(char op, SeaValue a, SeaValue b, const char **err);  /* + - * / % ^ */
SeaBigInt *sea_bigint_negate(const SeaBigInt *b, bool abs_only);
/* An integer view of v (bigint, int or a finite integral number). */
bool       sea_bigint_of(SeaValue v, SeaBigInt **out);    /* new bigint, or false */
/* == between a bigint and anything (int and number compare by value). */
bool       sea_bigint_equal(SeaValue a, SeaValue b);

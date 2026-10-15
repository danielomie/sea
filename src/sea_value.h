#pragma once
#include "sea_common.h"

/*
 * NaN-boxing value representation.
 *
 * A 64-bit double that is a quiet NaN has the pattern:
 *   exponent all 1s (11 bits), fraction non-zero, MSB of fraction = 1 (quiet)
 *
 * We hijack NaN space by using the sign bit + quiet NaN:
 *   Base pattern: 0xFFF8000000000000 (sign=1, exp=all-1, quiet bit=1)
 *
 * Type tags live in bits [50:48] (3 bits = 8 possible types):
 *   TAG 0 = NIL
 *   TAG 1 = BOOL  (bit 0 of lower 48 = bool value)
 *   TAG 2 = OBJ   (lower 48 bits = pointer)
 *   TAG 3 = INT   (lower 48 bits = signed integer, sign-extended)
 *
 * Any value that is NOT one of these NaN patterns is a real double.
 */

// Base NaN pattern (quiet NaN with sign bit set)
#define SEA_NAN_BASE   UINT64_C(0xFFF8000000000000)
// Type mask in bits [50:48]
#define SEA_TAG_MASK   UINT64_C(0x0007000000000000)
// Lower 48 bits for payload
#define SEA_PTR_MASK   UINT64_C(0x0000FFFFFFFFFFFF)

// Type tags
#define SEA_TAG_NIL    UINT64_C(0x0000000000000000)
#define SEA_TAG_BOOL   UINT64_C(0x0001000000000000)
#define SEA_TAG_OBJ    UINT64_C(0x0002000000000000)
#define SEA_TAG_INT    UINT64_C(0x0003000000000000)

// Sentinel values
#define SEA_NIL_VAL    (SEA_NAN_BASE | SEA_TAG_NIL)
#define SEA_TRUE_VAL   (SEA_NAN_BASE | SEA_TAG_BOOL | UINT64_C(1))
#define SEA_FALSE_VAL  (SEA_NAN_BASE | SEA_TAG_BOOL)

// Check if a value is in NaN-tag space (not a real double)
SEA_INLINE bool sea_is_tagged(SeaValue v) {
    // All tagged values have the top 13 bits = 0xFFF8 or 0xFFF9..0xFFFF
    // Real doubles: exponent bits not all 1s, or fraction = 0 (infinity), or quiet bit = 0
    // We use the sign+quiet pattern: top 13 bits = 1_11111111111_1 = 0x1FFE (in 13-bit form)
    // Simpler: if (v & 0x7FF0000000000000) == 0x7FF0000000000000 it's a NaN or infinity
    // For our purposes: tagged if top bits match NAN_BASE
    return (v & SEA_NAN_BASE) == SEA_NAN_BASE;
}

// ---- Type checks ----

SEA_INLINE bool sea_is_number(SeaValue v) {
    // A real double: NOT in our NaN-tag space
    // We detect by checking if top 13 bits are 0xFFF8 pattern
    // Actually: if (v & NAN_BASE) != NAN_BASE => it's a double
    // But NaN_BASE has sign=1, exp=all1, quiet=1 => top 12 bits = 0xFFF, bit 51 = 1
    // Normal doubles don't have exponent all-1s and sign=1 and fraction bit 51=1 simultaneously
    // unless they're this specific NaN pattern
    return (v & SEA_NAN_BASE) != SEA_NAN_BASE;
}

SEA_INLINE bool sea_is_nil(SeaValue v) {
    return v == SEA_NIL_VAL;
}

SEA_INLINE bool sea_is_bool(SeaValue v) {
    return (v & (SEA_NAN_BASE | SEA_TAG_MASK)) == (SEA_NAN_BASE | SEA_TAG_BOOL);
}

SEA_INLINE bool sea_is_obj(SeaValue v) {
    return (v & (SEA_NAN_BASE | SEA_TAG_MASK)) == (SEA_NAN_BASE | SEA_TAG_OBJ);
}

SEA_INLINE bool sea_is_int(SeaValue v) {
    return (v & (SEA_NAN_BASE | SEA_TAG_MASK)) == (SEA_NAN_BASE | SEA_TAG_INT);
}

// ---- Constructors ----

SEA_INLINE SeaValue sea_number_val(double n) {
    SeaValue v;
    memcpy(&v, &n, sizeof(v));
    return v;
}

SEA_INLINE SeaValue sea_int_val(int64_t i) {
    // Store lower 48 bits; sign-extend is handled by accessor
    return SEA_NAN_BASE | SEA_TAG_INT | (SeaValue)(i & SEA_PTR_MASK);
}

SEA_INLINE SeaValue sea_obj_val(SeaObject *obj) {
    return SEA_NAN_BASE | SEA_TAG_OBJ | ((SeaValue)(uintptr_t)obj & SEA_PTR_MASK);
}

SEA_INLINE SeaValue sea_bool_val(bool b) {
    return b ? SEA_TRUE_VAL : SEA_FALSE_VAL;
}

// ---- Accessors ----

SEA_INLINE double sea_as_number(SeaValue v) {
    double n;
    memcpy(&n, &v, sizeof(n));
    return n;
}

SEA_INLINE int64_t sea_as_int(SeaValue v) {
    /* Shift out the 16-bit NaN tag, then arithmetic right-shift back:
     * this sign-extends bit 47 (the MSB of the 48-bit payload) to 64 bits,
     * branchless and cheaper than the mask+conditional approach. */
    return (int64_t)(v << 16) >> 16;
}

SEA_INLINE bool sea_as_bool(SeaValue v) {
    return (v & 1) != 0;
}

SEA_INLINE SeaObject* sea_as_obj(SeaValue v) {
    return (SeaObject*)(uintptr_t)(v & SEA_PTR_MASK);
}

// ---- Helpers ----

// Truthiness: nil and false are falsy, everything else is truthy
SEA_INLINE bool sea_is_truthy(SeaValue v) {
    if (v == SEA_NIL_VAL) return false;
    if (v == SEA_FALSE_VAL) return false;
    return true;
}

// Value equality
SEA_INLINE bool sea_val_equal(SeaValue a, SeaValue b) {
    if (sea_is_number(a) && sea_is_number(b)) {
        return sea_as_number(a) == sea_as_number(b);
    }
    // Cross-type numeric equality: int and float with the same mathematical value
    if (sea_is_int(a) && sea_is_number(b)) {
        return (double)sea_as_int(a) == sea_as_number(b);
    }
    if (sea_is_number(a) && sea_is_int(b)) {
        return sea_as_number(a) == (double)sea_as_int(b);
    }
    return a == b;
}

// Type name for error messages
const char* sea_type_name(SeaValue v);

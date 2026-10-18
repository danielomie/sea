/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * sea_utf8.c — UTF-8 decoding/encoding, validation, simple case mapping and
 * terminal widths (see sea_utf8.h).
 */

#include "sea_utf8.h"

int sea_utf8_decode(const unsigned char *s, size_t n, uint32_t *cp) {
    if (n == 0) return 0;
    unsigned char c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if (c >= 0xC2 && c <= 0xDF) {
        if (n < 2 || (s[1] & 0xC0) != 0x80) return 0;
        *cp = ((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if (n < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        uint32_t v = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (v < 0x800 || (v >= 0xD800 && v <= 0xDFFF)) return 0;
        *cp = v;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (n < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        uint32_t v = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
                     ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        if (v < 0x10000 || v > 0x10FFFF) return 0;
        *cp = v;
        return 4;
    }
    return 0;
}

int sea_utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp > 0x10FFFF) return 0;
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t sea_utf8_check(const char *s, size_t n) {
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0;
    while (i < n) {
        if (u[i] < 0x80) { i++; continue; }
        uint32_t cp;
        int k = sea_utf8_decode(u + i, n - i, &cp);
        if (!k) return i;
        i += (size_t)k;
    }
    return n;
}

size_t sea_utf8_next(const char *s, size_t n, size_t i) {
    if (i >= n) return n;
    const unsigned char *u = (const unsigned char *)s;
    if (u[i] < 0x80) return i + 1;
    uint32_t cp;
    int k = sea_utf8_decode(u + i, n - i, &cp);
    return i + (k ? (size_t)k : 1);
}

size_t sea_utf8_len(const char *s, size_t n) {
    size_t count = 0;
    for (size_t i = 0; i < n; i = sea_utf8_next(s, n, i)) count++;
    return count;
}

/* ---- Case mapping ------------------------------------------------------------
 * Ranges where upper and lower case letters alternate (U+0100 Ā, U+0101 ā, ...):
 * `upper_first` says whether the range's first code point is the capital. */

typedef struct { uint32_t lo, hi; uint8_t upper_even; } Pairs;

static const Pairs PAIRS[] = {
    { 0x0100, 0x012F, 1 }, { 0x0132, 0x0137, 1 }, { 0x0139, 0x0148, 0 }, { 0x014A, 0x0177, 1 },
    { 0x0179, 0x017E, 0 }, { 0x01CD, 0x01DC, 0 }, { 0x01DE, 0x01EF, 1 }, { 0x01F8, 0x021F, 1 },
    { 0x0222, 0x0233, 1 }, { 0x0460, 0x0481, 1 }, { 0x048A, 0x04BF, 1 }, { 0x04C1, 0x04CE, 0 },
    { 0x04D0, 0x052F, 1 }, { 0x1E00, 0x1E95, 1 }, { 0x1EA0, 0x1EFF, 1 },
};

/* Ranges mapped by a fixed offset: [lo, hi] (capitals) ↔ [lo + d, hi + d]. */
typedef struct { uint32_t lo, hi; int32_t d; } Shift;

static const Shift SHIFTS[] = {
    { 0x0041, 0x005A, 32 }, { 0x00C0, 0x00D6, 32 }, { 0x00D8, 0x00DE, 32 },
    { 0x0391, 0x03A1, 32 }, { 0x03A3, 0x03A9, 32 }, { 0x0388, 0x038A, 37 }, { 0x038E, 0x038F, 63 },
    { 0x0410, 0x042F, 32 }, { 0x0400, 0x040F, 80 }, { 0x0531, 0x0556, 48 },
    { 0xFF21, 0xFF3A, 32 },
};

/* One-off pairs: capital, small. */
static const uint32_t SINGLES[][2] = {
    { 0x0178, 0x00FF }, { 0x0386, 0x03AC }, { 0x038C, 0x03CC }, { 0x04C0, 0x04CF },
    { 0x1E9E, 0x00DF },
};

uint32_t sea_utf8_upper(uint32_t cp) {
    if (cp < 0x80) return (cp >= 'a' && cp <= 'z') ? cp - 32 : cp;
    for (size_t i = 0; i < sizeof SHIFTS / sizeof *SHIFTS; i++)
        if (cp >= SHIFTS[i].lo + (uint32_t)SHIFTS[i].d && cp <= SHIFTS[i].hi + (uint32_t)SHIFTS[i].d)
            return cp - (uint32_t)SHIFTS[i].d;
    for (size_t i = 0; i < sizeof PAIRS / sizeof *PAIRS; i++)
        if (cp >= PAIRS[i].lo && cp <= PAIRS[i].hi)
            return ((cp & 1) == (PAIRS[i].upper_even ? 1u : 0u)) ? cp - 1 : cp;
    switch (cp) {
        case 0x00B5: return 0x039C;          /* µ → Μ */
        case 0x0131: return 'I';             /* ı */
        case 0x017F: return 'S';             /* ſ */
        case 0x03C2: return 0x03A3;          /* final ς → Σ */
        default: break;
    }
    for (size_t i = 0; i < sizeof SINGLES / sizeof *SINGLES; i++)
        if (cp == SINGLES[i][1] && cp != 0x00DF) return SINGLES[i][0];
    return cp;
}

uint32_t sea_utf8_lower(uint32_t cp) {
    if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    for (size_t i = 0; i < sizeof SHIFTS / sizeof *SHIFTS; i++)
        if (cp >= SHIFTS[i].lo && cp <= SHIFTS[i].hi) return cp + (uint32_t)SHIFTS[i].d;
    for (size_t i = 0; i < sizeof PAIRS / sizeof *PAIRS; i++)
        if (cp >= PAIRS[i].lo && cp <= PAIRS[i].hi)
            return ((cp & 1) == (PAIRS[i].upper_even ? 0u : 1u)) ? cp + 1 : cp;
    if (cp == 0x0130) return 'i';            /* İ */
    for (size_t i = 0; i < sizeof SINGLES / sizeof *SINGLES; i++)
        if (cp == SINGLES[i][0]) return SINGLES[i][1];
    return cp;
}

/* ---- Terminal width ---------------------------------------------------------- */

typedef struct { uint32_t lo, hi; } Range;

/* Combining marks and zero-width characters (the common blocks). */
static const Range ZERO[] = {
    { 0x0300, 0x036F }, { 0x0483, 0x0489 }, { 0x0591, 0x05BD }, { 0x05BF, 0x05BF }, { 0x05C1, 0x05C2 },
    { 0x05C4, 0x05C5 }, { 0x05C7, 0x05C7 }, { 0x0610, 0x061A }, { 0x064B, 0x065F }, { 0x0670, 0x0670 },
    { 0x06D6, 0x06DC }, { 0x06DF, 0x06E4 }, { 0x06E7, 0x06E8 }, { 0x06EA, 0x06ED }, { 0x0900, 0x0902 },
    { 0x093A, 0x093A }, { 0x093C, 0x093C }, { 0x0941, 0x0948 }, { 0x094D, 0x094D }, { 0x0951, 0x0957 },
    { 0x0E31, 0x0E31 }, { 0x0E34, 0x0E3A }, { 0x0E47, 0x0E4E }, { 0x1160, 0x11FF }, { 0x1AB0, 0x1AFF },
    { 0x1DC0, 0x1DFF }, { 0x200B, 0x200F }, { 0x202A, 0x202E }, { 0x2060, 0x2064 }, { 0x20D0, 0x20FF },
    { 0x302A, 0x302D }, { 0x3099, 0x309A }, { 0xFE00, 0xFE0F }, { 0xFE20, 0xFE2F }, { 0xFEFF, 0xFEFF },
    { 0x1F3FB, 0x1F3FF }, { 0xE0001, 0xE007F }, { 0xE0100, 0xE01EF },
};

/* East Asian wide/fullwidth characters and emoji presented wide. */
static const Range WIDE[] = {
    { 0x1100, 0x115F }, { 0x231A, 0x231B }, { 0x2329, 0x232A }, { 0x23E9, 0x23EC }, { 0x23F0, 0x23F0 },
    { 0x23F3, 0x23F3 }, { 0x25FD, 0x25FE }, { 0x2614, 0x2615 }, { 0x2648, 0x2653 }, { 0x267F, 0x267F },
    { 0x2693, 0x2693 }, { 0x26A1, 0x26A1 }, { 0x26AA, 0x26AB }, { 0x26BD, 0x26BE }, { 0x26C4, 0x26C5 },
    { 0x26CE, 0x26CE }, { 0x26D4, 0x26D4 }, { 0x26EA, 0x26EA }, { 0x26F2, 0x26F3 }, { 0x26F5, 0x26F5 },
    { 0x26FA, 0x26FA }, { 0x26FD, 0x26FD }, { 0x2705, 0x2705 }, { 0x270A, 0x270B }, { 0x2728, 0x2728 },
    { 0x274C, 0x274C }, { 0x274E, 0x274E }, { 0x2753, 0x2755 }, { 0x2757, 0x2757 }, { 0x2795, 0x2797 },
    { 0x27B0, 0x27B0 }, { 0x27BF, 0x27BF }, { 0x2B1B, 0x2B1C }, { 0x2B50, 0x2B50 }, { 0x2B55, 0x2B55 },
    { 0x2E80, 0x303E }, { 0x3041, 0x3247 }, { 0x3250, 0x4DBF }, { 0x4E00, 0xA4CF }, { 0xA960, 0xA97F },
    { 0xAC00, 0xD7A3 }, { 0xF900, 0xFAFF }, { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE6F }, { 0xFF00, 0xFF60 },
    { 0xFFE0, 0xFFE6 }, { 0x16FE0, 0x16FE4 }, { 0x17000, 0x18CFF }, { 0x1B000, 0x1B2FF }, { 0x1F004, 0x1F004 },
    { 0x1F0CF, 0x1F0CF }, { 0x1F18E, 0x1F18E }, { 0x1F191, 0x1F19A }, { 0x1F200, 0x1F251 },
    { 0x1F300, 0x1F320 }, { 0x1F32D, 0x1F335 }, { 0x1F337, 0x1F37C }, { 0x1F37E, 0x1F393 },
    { 0x1F3A0, 0x1F3CA }, { 0x1F3CF, 0x1F3D3 }, { 0x1F3E0, 0x1F3F0 }, { 0x1F3F4, 0x1F3F4 },
    { 0x1F3F8, 0x1F43E }, { 0x1F440, 0x1F440 }, { 0x1F442, 0x1F4FC }, { 0x1F4FF, 0x1F53D },
    { 0x1F54B, 0x1F54E }, { 0x1F550, 0x1F567 }, { 0x1F57A, 0x1F57A }, { 0x1F595, 0x1F596 },
    { 0x1F5A4, 0x1F5A4 }, { 0x1F5FB, 0x1F64F }, { 0x1F680, 0x1F6C5 }, { 0x1F6CC, 0x1F6CC },
    { 0x1F6D0, 0x1F6D2 }, { 0x1F6D5, 0x1F6D7 }, { 0x1F6DC, 0x1F6DF }, { 0x1F6EB, 0x1F6EC },
    { 0x1F6F4, 0x1F6FC }, { 0x1F7E0, 0x1F7EB }, { 0x1F7F0, 0x1F7F0 }, { 0x1F90C, 0x1F93A },
    { 0x1F93C, 0x1F945 }, { 0x1F947, 0x1F9FF }, { 0x1FA70, 0x1FAFF }, { 0x20000, 0x2FFFD },
    { 0x30000, 0x3FFFD },
};

static bool in_ranges(const Range *r, size_t n, uint32_t cp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < r[mid].lo) hi = mid;
        else if (cp > r[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

int sea_utf8_cpwidth(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return -1;
    if (cp < 0x300) return 1;
    if (in_ranges(ZERO, sizeof ZERO / sizeof *ZERO, cp)) return 0;
    if (in_ranges(WIDE, sizeof WIDE / sizeof *WIDE, cp)) return 2;
    return 1;
}

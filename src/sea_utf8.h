#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * UTF-8 helpers shared by the lexer, the `utf8` module and native libraries
 * (json, xml, yaml, console). Strings are byte strings; these functions read
 * them as UTF-8. "Valid" means well-formed per RFC 3629: no overlong forms,
 * no surrogates (U+D800..U+DFFF), nothing above U+10FFFF.
 */

/* Length (1..4) of the valid sequence at s (n bytes available) and its code
 * point in *cp; 0 if the bytes there are not a valid sequence. */
int      sea_utf8_decode(const unsigned char *s, size_t n, uint32_t *cp);

/* Encode cp into out (at least 4 bytes); the byte count, or 0 when cp is a
 * surrogate or above U+10FFFF. */
int      sea_utf8_encode(uint32_t cp, char *out);

/* Byte index of the first invalid sequence, or n when all of s is valid. */
size_t   sea_utf8_check(const char *s, size_t n);

/* Characters in s; an invalid byte counts as one character. */
size_t   sea_utf8_len(const char *s, size_t n);

/* Byte index of the character after the one starting at i (an invalid byte
 * is one character); n at the end. */
size_t   sea_utf8_next(const char *s, size_t n, size_t i);

/* Simple (one-to-one) case mapping: Latin, Greek, Cyrillic and Armenian
 * letters; every other code point maps to itself. */
uint32_t sea_utf8_upper(uint32_t cp);
uint32_t sea_utf8_lower(uint32_t cp);

/* Columns a code point takes in a terminal: 0 for combining marks and
 * zero-width characters, 2 for East Asian wide and fullwidth characters
 * and emoji, 1 otherwise (-1 for other control characters). */
int      sea_utf8_cpwidth(uint32_t cp);

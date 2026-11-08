/*
    aer_ctype.h
    Character classification (freestanding replacement for <ctype.h>).

    Why do we need our own?
    AerOS is built with -ffreestanding and linked with -nostdlib, so there is no
    C library underneath us at all: no glibc, no newlib, nothing. In a freestanding
    build the compiler only promises a handful of headers that are pure definitions
    (<stddef.h>, <stdint.h>, <stdbool.h>, <stdarg.h> and friends). Anything that needs
    real code behind it, like isdigit() or toupper(), we have to write ourselves.
    See https://wiki.osdev.org/C_Library for the bigger picture.

    How these work:
    - Everything here is plain 7-bit ASCII. There are no locales, so 'a'..'z' are the
      only lowercase letters and anything above 127 is "not a letter, not a digit".
    - The functions are "static inline" in the header, so every .c file that includes
      this gets its own private copy which the compiler normally folds straight into
      the caller. No .c file is needed and there are no duplicate symbols at link time.
    - Like the standard ones they take an int, not a char. On x86 a plain char is
      signed, so a byte like 0xE9 would arrive as -23. Callers therefore pass
      (unsigned char)c, which is why you'll see that cast all over the codebase.
    - They return non-zero for "yes" and 0 for "no" (here it is always exactly 1 or 0).

    A handy ASCII map for reading the ranges below:
        0..31  control characters (tab = 9, newline = 10, carriage return = 13)
        32     space
        48..57 '0'..'9'      65..90 'A'..'Z'      97..122 'a'..'z'
        127    DEL (another control character)
*/
#ifndef AER_CTYPE_H
#define AER_CTYPE_H

// '0'..'9', digits are stored one after another in ASCII so a range check is enough
static inline int isdigit(int c)  { return c >= '0' && c <= '9'; }
// 'A'..'Z'
static inline int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
// 'a'..'z'
static inline int islower(int c)  { return c >= 'a' && c <= 'z'; }
// any letter, upper or lower case
static inline int isalpha(int c)  { return isupper(c) || islower(c); }
// a letter or a digit (what you'd allow in a name or identifier)
static inline int isalnum(int c)  { return isalpha(c) || isdigit(c); }
// a hexadecimal digit: 0-9, a-f or A-F (used by strtol when parsing "0x1F")
static inline int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
// whitespace: space, or the run 9..13 which is '\t' tab, '\n' newline, '\v' vertical tab,
// '\f' form feed and '\r' carriage return
static inline int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
// control characters: the invisible codes 0..31 plus DEL (127)
static inline int iscntrl(int c)  { return (c >= 0 && c < 32) || c == 127; }
// anything that puts something on screen, including the space (32..126)
static inline int isprint(int c)  { return c >= 32 && c < 127; }
// like isprint, but the space doesn't count because it leaves no visible mark (33..126)
static inline int isgraph(int c)  { return c > 32 && c < 127; }
// punctuation: visible characters that aren't letters or digits, e.g. ! . / { ~
static inline int ispunct(int c)  { return isgraph(c) && !isalnum(c); }
// lower case to upper case, anything else comes back unchanged
// (in ASCII 'a' - 'A' is 32, so we just shift the letter down by that distance)
static inline int toupper(int c)  { return islower(c) ? c - 'a' + 'A' : c; }
// upper case to lower case, anything else comes back unchanged
static inline int tolower(int c)  { return isupper(c) ? c - 'A' + 'a' : c; }

#endif /* AER_CTYPE_H */

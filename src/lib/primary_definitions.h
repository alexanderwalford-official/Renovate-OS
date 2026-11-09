/*
    primary_definitions.h
    The core helper library. Including this gives you the freestanding C library
    replacements (aer_ctype / aer_string / aer_stdlib / aer_stdio) plus the AerOS
    specific helpers below.

    Think of this as the kernel's "include everything" header. Most modules (the terminal,
    the process manager, the file system, AerLang) include just this one file and get:
    - the fixed size integer types (uint8_t, uint32_t...), bool / true / false, size_t / NULL
    - the whole of our little C library: strings, memory, number conversion, malloc, prtf
    - the VGA console functions, so anything can draw on the screen
    - the AerOS string helpers below, which predate the C library and give the kernel a
      slightly friendlier, Python flavoured way of handling strings (contains, split, remove...)

    Why lib/ has its own C library at all is explained at the top of aer_ctype.h and
    aer_string.h.
*/
#ifndef SYS_PRIMARY_DEFINITIONS_H
#define SYS_PRIMARY_DEFINITIONS_H

// these come with the compiler itself and are pure definitions (no code), so they are
// safe to use even in a freestanding kernel
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
// our own C library replacements
#include "aer_ctype.h"
#include "aer_string.h"
#include "aer_stdlib.h"
#include "aer_stdio.h"
// the text mode screen and console
#include "../drivers/video/VGA_linear.h"

// pi to 5 decimal places, a double
// note: any floating point maths would run on the x87 FPU (SSE is switched off), and prtf can't print floats
#ifndef PI
#define PI 3.14159
#endif

// "for each" loop, only works on real arrays (not pointers), e.g.
//     int values[3] = {1, 2, 3};
//     int* item;
//     foreach(item, values) { prtf("%d\n", *item); }
//
// How the macro works (it's a well known trick, two nested for loops):
// - the OUTER loop counts foreach_count from 0 up to the number of elements, which it works out
//   with sizeof(array) / sizeof(one element) (that's why a pointer won't do, sizeof a pointer is
//   just 4 bytes on 32 bit x86 no matter how much it points at)
// - the INNER loop's only job is to set item = &array[foreach_count] and then run your loop body
//   exactly once. It uses the foreach_keep flag as its condition, flipping it to 0 after one pass.
// - the outer loop flips foreach_keep back to 1 before the next element.
// - the clever bit is what happens on "break": it only leaves the INNER loop, and skips the flip,
//   so foreach_keep is still 1. The outer loop's increment then flips it to 0, and the outer
//   condition (foreach_keep && ...) sees that and stops. So break leaves the whole foreach, just
//   like it would a normal loop. "continue" works as expected too.
// item must be a pointer to the element type, declared before the loop; the counters are declared
// inside the for, so they can't clash with your own variables outside it.
#define foreach(item, array) \
    for(int foreach_keep = 1, \
            foreach_count = 0,\
            foreach_size = (int)(sizeof (array) / sizeof *(array)); \
        foreach_keep && foreach_count != foreach_size; \
        foreach_keep = !foreach_keep, foreach_count++) \
      for(item = (array) + foreach_count; foreach_keep; foreach_keep = !foreach_keep)

// true if target appears anywhere inside string (false if either is NULL)
// e.g. contains_str("hello world", "lo w") is true; an empty target is always found
bool contains_str(const char* string, const char* target);

// StrCombine and toArray return a shared buffer, the next call overwrites it (copy it if you need to keep it)

// join str1 and str2 into one string (NULL counts as ""), up to 1023 characters, the rest is cut off
// nesting is fine: StrCombine(StrCombine("a", "b"), "c") gives "abc"
char* StrCombine(const char* str1, const char* str2);
// turn a number into decimal text, e.g. toArray(-42) gives "-42"
// gotcha: prtf("%s %s", toArray(1), toArray(2)) prints the same number twice, both point at the one buffer
char* toArray(int number);

// 1 when the strings match, 0 when they don't (or either is NULL)
// the opposite way round to scmp (which gives 0 for a match), so it reads naturally:
// if (seq(command, "help")) { ... }
int seq(const char* string_a, const char* string_b);

// split string in place on split_char into at most max_parts pieces, storing a pointer to each in parts
// (parts needs room for max_parts pointers), returns how many parts were made
// the last part keeps the rest of the line, so split("copy a b", ' ', parts, 2) gives "copy" and "a b"
int split(char* string, char split_char, char** parts, int max_parts);
// remove target from string in place, at most c times (c <= 0 means every time), returns string
char* StrRemove(char* string, const char* target, int c);
// strip whitespace off both ends in place, returns the new start of the string (use that pointer!)
char* trim(char* string);
// does nothing and returns straight away (an old placeholder, nothing calls it at the moment)
void ret(void);

#endif /* SYS_PRIMARY_DEFINITIONS_H */

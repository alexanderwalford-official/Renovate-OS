/*
    primary_definitions.h
    The core helper library. Including this gives you the freestanding C library
    replacements (ren_ctype / ren_string / ren_stdlib / ren_stdio) plus the Renovate
    specific helpers below.
*/
#ifndef SYS_PRIMARY_DEFINITIONS_H
#define SYS_PRIMARY_DEFINITIONS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ren_ctype.h"
#include "ren_string.h"
#include "ren_stdlib.h"
#include "ren_stdio.h"
#include "../drivers/video/VGA_linear.h"

#ifndef PI
#define PI 3.14159
#endif

// "for each" loop, only works on real arrays (not pointers), e.g.
//     int values[3] = {1, 2, 3};
//     int* item;
//     foreach(item, values) { printf("%d\n", *item); }
#define foreach(item, array) \
    for(int foreach_keep = 1, \
            foreach_count = 0,\
            foreach_size = (int)(sizeof (array) / sizeof *(array)); \
        foreach_keep && foreach_count != foreach_size; \
        foreach_keep = !foreach_keep, foreach_count++) \
      for(item = (array) + foreach_count; foreach_keep; foreach_keep = !foreach_keep)

bool contains_str(const char* string, const char* target);

// StrCombine and toArray return a shared buffer, the next call overwrites it (copy it if you need to keep it)
char* StrCombine(const char* str1, const char* str2);
char* toArray(int number);

// 1 when the strings match, 0 when they don't (or either is NULL)
int StrCompare(const char* string_a, const char* string_b);

int split(char* string, char split_char, char** parts, int max_parts);
char* StrRemove(char* string, const char* target, int c);
char* StrTrim(char* string);
void ret(void);

#endif /* SYS_PRIMARY_DEFINITIONS_H */

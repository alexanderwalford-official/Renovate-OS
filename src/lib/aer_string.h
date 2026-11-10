/*
    aer_string.h
    String and memory functions (freestanding replacement for <string.h>).

    NOTE: GCC may emit calls to memcpy / memset / memmove / memcmp by itself (for example
    when copying structs), so these must exist with exactly these names and signatures.

    Why does GCC call functions we never wrote a call to?
    Even in a -ffreestanding build, GCC assumes these four memory functions exist
    (the GCC manual says so in its "Standards" section). When you write something like
    "big_struct a = b;" or "char buf[256] = {0};" the compiler is free to turn that into
    a call to memcpy or memset rather than writing out a long run of mov instructions.
    So if they were missing, or had a slightly different name or signature, the kernel
    would fail to link (or worse, misbehave) for reasons that are hard to spot.

    Two compiler flags in the Makefile work together with this file:
    - -fno-builtin stops GCC from swapping OUR calls to slen() and friends for its own
      built in versions, so the code here is always the code that runs.
    - -fno-tree-loop-distribute-patterns stops GCC noticing that the loop inside our
      memset "looks like a memset" and replacing it with... a call to memset. That
      would make memset call itself forever!

    A quick word on "restrict": it is a promise to the compiler that the two pointers
    never point into the same memory, which lets it optimise more freely. memcpy and
    the str*cpy / str*cat family take restrict pointers, so overlapping source and
    destination is not allowed for them. If your regions might overlap, use memmove.

    All the strings here are ordinary C strings: an array of chars ending in a '\0'
    byte (the "terminator"). Lengths never include that terminator.

    Handy references:
        https://wiki.osdev.org/C_Library
        https://en.cppreference.com/w/c/string/byte
*/
#ifndef AER_STRING_H
#define AER_STRING_H

#include <stddef.h>

// memory

// fill count bytes at dest with the byte value (only the low 8 bits of value are used), returns dest
void* memset(void* dest, int value, size_t count);
// copy count bytes from src to dest, returns dest (the regions must NOT overlap)
void* memcpy(void* restrict dest, const void* restrict src, size_t count);
// copy count bytes from src to dest, returns dest
void* memmove(void* dest, const void* src, size_t count); // safe when the regions overlap
// compare count bytes: 0 if equal, otherwise <0 or >0 depending on the first byte that differs
int memcmp(const void* a, const void* b, size_t count);
// find the first byte equal to c in the first count bytes of s, or NULL if it isn't there
void* memchr(const void* s, int c, size_t count);

// length

// number of characters before the '\0' terminator
size_t slen(const char* s);
// like slen but never looks at more than max characters (safe on unterminated buffers)
size_t strnlen(const char* s, size_t max);

// comparison
// all of these return 0 when the strings match, <0 when a sorts first, >0 when b sorts first

// compare two strings
int scmp(const char* a, const char* b);
// compare at most the first n characters
int strncmp(const char* a, const char* b, size_t n);
// compare ignoring upper / lower case, so "Hello" matches "hELLO"
int strcasecmp(const char* a, const char* b);
// compare at most n characters ignoring case
int strncasecmp(const char* a, const char* b, size_t n);

// copying
// dest must already have enough room, these don't allocate anything

// copy src (including its '\0') into dest, returns dest (no size check at all!)
char* strcpy(char* restrict dest, const char* restrict src);
// copy exactly n characters, padding with '\0', but leaves dest UNterminated if src is too long
char* strncpy(char* restrict dest, const char* restrict src, size_t n);
// copy into a buffer of size bytes, always terminated, returns slen(src) (>= size means it was cut short)
size_t scpy(char* restrict dest, const char* restrict src, size_t size);
// append src to the end of dest, returns dest (no size check at all!)
char* strcat(char* restrict dest, const char* restrict src);
// append at most n characters of src and then a '\0', returns dest
char* strncat(char* restrict dest, const char* restrict src, size_t n);
// append into a buffer of size bytes, always terminated, returns the length it tried to make
size_t scat(char* restrict dest, const char* restrict src, size_t size);

// searching

// first occurrence of the character c in s (c may be '\0' to find the end), or NULL
char* strchr(const char* s, int c);
// last occurrence of the character c in s, or NULL
char* strrchr(const char* s, int c);
// first place needle appears inside haystack, or NULL (an empty needle matches straight away)
char* strstr(const char* haystack, const char* needle);
// how many characters at the start of s are all in the set accept
size_t strspn(const char* s, const char* accept);
// how many characters at the start of s are all NOT in the set reject
size_t strcspn(const char* s, const char* reject);
// first character in s that is in the set accept, or NULL
char* strpbrk(const char* s, const char* accept);
// split a string into tokens, keeping its place in *saveptr (safe to use from several places at once)
char* strtok_r(char* str, const char* delim, char** saveptr);
// split a string into tokens, keeping its place in a hidden static (only one job at a time)
char* strtok(char* str, const char* delim);

// non-standard extras, all work in place

// reverse the characters of s, returns s
char* strrev(char* s);
// make every letter in s upper case, returns s
char* strupr(char* s);
// make every letter in s lower case, returns s
char* strlwr(char* s);

// How different two short strings are: the fewest single character changes that turn one into
// the other, where a change is adding a character, taking one away, swapping one for another,
// or swapping two next to each other round. Upper and lower case don't count.
//     sdist("del", "del") is 0,  sdist("dle", "del") is 1,  sdist("notez.txt", "notes.txt") is 1
// For "did you mean...?" (names): strings of 32 characters or more just give 99
int sdist(const char* a, const char* b);

#endif /* AER_STRING_H */

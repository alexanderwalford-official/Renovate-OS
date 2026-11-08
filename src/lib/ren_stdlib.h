/*
    ren_stdlib.h
    General utilities (freestanding replacement for <stdlib.h>):
    number conversion, random numbers, sorting and a kernel heap (malloc / free).

    Like the rest of lib/, this exists because the kernel is built freestanding with
    no C library to lean on (see ren_ctype.h for the longer explanation). The names and
    signatures match the standard ones, so code here reads just like normal C.

    What's inside:
    - integer helpers: abs, itoa / utoa (number to text), strtol / strtoul / atoi (text to number)
    - a small pseudo random number generator: rand / srand
    - qsort, used by the process manager to line its processes up shortest first
    - the kernel heap: malloc / free / calloc / realloc over a fixed 4MB pool,
      plus strdup / strndup which copy a string onto the heap
    The heap's inner workings are explained at length in ren_stdlib.c.

    What's missing compared with a hosted libc: there's no errno (failures are reported
    through return values only), no exit() / abort() (there's no process to exit back
    to, the kernel IS the program), no getenv(), and nothing to do with floating point.

    Handy references:
        https://wiki.osdev.org/C_Library
        https://wiki.osdev.org/Memory_Allocation
        https://en.cppreference.com/w/c/program
*/
#ifndef REN_STDLIB_H
#define REN_STDLIB_H

#include <stddef.h>

// <limits.h> pulls in the host's C library headers, so take the limits from the compiler instead
// GCC predefines __INT_MAX__ and __LONG_MAX__ for whatever target it is compiling for,
// so these are always right for 32 bit x86 (where int and long are both 32 bits).
// The MIN values follow from two's complement: the negative range is one bigger than the positive one.
// The unsigned MAX values are "every bit set", which is twice the signed MAX plus one.
// The #ifndef means that if some other header already defined them we quietly use those.
#ifndef INT_MAX
#define INT_MAX   __INT_MAX__
#define INT_MIN   (-INT_MAX - 1)
#define UINT_MAX  (INT_MAX * 2U + 1U)
#define LONG_MAX  __LONG_MAX__
#define LONG_MIN  (-LONG_MAX - 1L)
#define ULONG_MAX (LONG_MAX * 2UL + 1UL)
#endif

// the usual "it worked" / "it didn't" status codes
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
// the biggest number rand() can return (rand gives 15 random bits, 0..32767)
#define RAND_MAX 32767

// smaller / bigger of two values
// gotcha: these are macros, so an argument gets evaluated twice. MIN(i++, 10) bumps i twice,
// and MIN(slow_function(), 10) calls it twice. Store such things in a variable first.
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
// number of elements in an array: total size in bytes divided by the size of one element
// gotcha: only works on a real array. On a pointer (including an array passed into a
// function, which turns into a pointer) it silently gives sizeof(pointer) / sizeof(element).
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

// integers

// absolute value (abs(INT_MIN) can't be represented and stays INT_MIN)
int abs(int value);
// absolute value of a long
long labs(long value);
// write value as text in base 2..36 into str, returns str (str needs up to 33 bytes for base 2)
char* utoa(unsigned int value, char* str, int base);
// like utoa but signed: base 10 gets a '-', other bases show the raw two's complement bits
char* itoa(int value, char* str, int base);
// parse a number from text, base 0 means "work it out" (0x.. hex, 0.. octal, else decimal)
// *endptr (if not NULL) is set to just after the number; out of range values clamp to LONG_MIN / LONG_MAX
long strtol(const char* nptr, char** endptr, int base);
// unsigned version of strtol, clamps to ULONG_MAX
unsigned long strtoul(const char* nptr, char** endptr, int base);
// quick decimal parse with no error reporting ("42abc" gives 42, "abc" gives 0)
int atoi(const char* s);
// atoi for a long
long atol(const char* s);

// random numbers
// not remotely secure, but fine for games and demos

// next pseudo random number, 0..RAND_MAX
int rand(void);
// restart the sequence from seed (the same seed always gives the same sequence)
void srand(unsigned int seed);

// sort count elements of size bytes each, compare returns <0, 0 or >0 like strcmp
// (it's really an insertion sort, which keeps equal elements in their original order)
void qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*));

// kernel heap

// allocate size bytes (16 byte aligned), or NULL if there's no room; the memory is NOT cleared
void* malloc(size_t size);
// give memory from malloc / calloc / realloc / strdup back; NULL is fine, bad pointers are ignored
void free(void* ptr);
// allocate count * size bytes and zero them, NULL on overflow or no room
void* calloc(size_t count, size_t size);
// resize an allocation, may move it (use the returned pointer); on failure the old one is untouched
void* realloc(void* ptr, size_t size);
// copy a string onto the heap (free() it when you're done)
char* strdup(const char* s);
// copy at most n characters of a string onto the heap, always terminated (free() it when done)
char* strndup(const char* s, size_t n);

// heap statistics (in bytes)

// the size of the whole heap pool
size_t heap_total(void);
// how much of it is currently handed out, block headers included
size_t heap_used(void);

#endif /* REN_STDLIB_H */

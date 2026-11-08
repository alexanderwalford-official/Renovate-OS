/*
    ren_stdlib.h
    General utilities (freestanding replacement for <stdlib.h>):
    number conversion, random numbers, sorting and a kernel heap (malloc / free).
*/
#ifndef REN_STDLIB_H
#define REN_STDLIB_H

#include <stddef.h>

// <limits.h> pulls in the host's C library headers, so take the limits from the compiler instead
#ifndef INT_MAX
#define INT_MAX   __INT_MAX__
#define INT_MIN   (-INT_MAX - 1)
#define UINT_MAX  (INT_MAX * 2U + 1U)
#define LONG_MAX  __LONG_MAX__
#define LONG_MIN  (-LONG_MAX - 1L)
#define ULONG_MAX (LONG_MAX * 2UL + 1UL)
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

// integers
int abs(int value);
long labs(long value);
char* utoa(unsigned int value, char* str, int base);
char* itoa(int value, char* str, int base);
long strtol(const char* nptr, char** endptr, int base);
unsigned long strtoul(const char* nptr, char** endptr, int base);
int atoi(const char* s);
long atol(const char* s);

// random numbers
int rand(void);
void srand(unsigned int seed);

void qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*));

// kernel heap
void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t count, size_t size);
void* realloc(void* ptr, size_t size);
char* strdup(const char* s);
char* strndup(const char* s, size_t n);

// heap statistics (in bytes)
size_t heap_total(void);
size_t heap_used(void);

#endif /* REN_STDLIB_H */

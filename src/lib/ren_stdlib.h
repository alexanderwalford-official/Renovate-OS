/*
    ren_stdlib.h
    General utilities (freestanding replacement for <stdlib.h>):
    number conversion, random numbers, sorting and a kernel heap (malloc / free).
*/
#ifndef REN_STDLIB_H
#define REN_STDLIB_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "ren_ctype.h"
#include "ren_string.h"

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

/* ---------------- integers ---------------- */

int abs(int value) {
    return value < 0 ? -value : value;
}

long labs(long value) {
    return value < 0 ? -value : value;
}

// convert an unsigned int to a string in any base from 2 to 36
char* utoa(unsigned int value, char* str, int base) {
    char* ptr = str;
    if (base < 2 || base > 36) {
        *str = '\0';
        return str;
    }
    do {
        *ptr++ = "0123456789abcdefghijklmnopqrstuvwxyz"[value % (unsigned int)base];
        value /= (unsigned int)base;
    } while (value);
    *ptr = '\0';
    return strrev(str);
}

// convert int to string (moved here from drivers/input/IO.h)
// negative numbers get a '-' in base 10, other bases show the two's complement bits
char* itoa(int value, char* str, int base)
{
    char* rc;
    char* ptr;
    char* low;
    // Check for supported base.
    if ( base < 2 || base > 36 )
    {
        *str = '\0';
        return str;
    }
    if ( value < 0 && base != 10 )
    {
        return utoa((unsigned int)value, str, base);
    }
    rc = ptr = str;
    // Set '-' for negative decimals.
    if ( value < 0 && base == 10 )
    {
        *ptr++ = '-';
    }
    // Remember where the numbers start.
    low = ptr;
    // The actual conversion.
    do
    {
        // Modulo is negative for negative value. This trick makes abs() unnecessary.
        *ptr++ = "zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz"[35 + value % base];
        value /= base;
    } while ( value );
    // Terminating the string.
    *ptr-- = '\0';
    // Invert the numbers.
    while ( low < ptr )
    {
        char tmp = *low;
        *low++ = *ptr;
        *ptr-- = tmp;
    }
    return rc;
}

// shared parser for strtol / strtoul
unsigned long ren_strtoul_core(const char* nptr, char** endptr, int base, int* negative, int* overflow) {
    const char* s = nptr;
    unsigned long acc = 0;
    int any = 0;

    *negative = 0;
    *overflow = 0;

    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '-' || *s == '+') {
        *negative = (*s == '-');
        s++;
    }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && isxdigit((unsigned char)s[2])) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = (s[0] == '0') ? 8 : 10;
    }
    if (base < 2 || base > 36) {
        if (endptr) {
            *endptr = (char*)nptr;
        }
        return 0;
    }

    unsigned long cutoff = ULONG_MAX / (unsigned long)base;
    unsigned long cutlim = ULONG_MAX % (unsigned long)base;
    for (;; s++) {
        int c = (unsigned char)*s;
        int digit;
        if (isdigit(c)) {
            digit = c - '0';
        } else if (isalpha(c)) {
            digit = tolower(c) - 'a' + 10;
        } else {
            break;
        }
        if (digit >= base) {
            break;
        }
        any = 1;
        if (acc > cutoff || (acc == cutoff && (unsigned long)digit > cutlim)) {
            *overflow = 1;
        } else {
            acc = acc * (unsigned long)base + (unsigned long)digit;
        }
    }
    if (endptr) {
        *endptr = (char*)(any ? s : nptr);
    }
    return acc;
}

long strtol(const char* nptr, char** endptr, int base) {
    int negative, overflow;
    unsigned long value = ren_strtoul_core(nptr, endptr, base, &negative, &overflow);
    if (negative) {
        if (overflow || value > (unsigned long)LONG_MAX + 1UL) {
            return LONG_MIN;
        }
        if (value == (unsigned long)LONG_MAX + 1UL) {
            return LONG_MIN;
        }
        return -(long)value;
    }
    if (overflow || value > (unsigned long)LONG_MAX) {
        return LONG_MAX;
    }
    return (long)value;
}

unsigned long strtoul(const char* nptr, char** endptr, int base) {
    int negative, overflow;
    unsigned long value = ren_strtoul_core(nptr, endptr, base, &negative, &overflow);
    if (overflow) {
        return ULONG_MAX;
    }
    return negative ? (unsigned long)(-(long)value) : value;
}

int atoi(const char* s) {
    return (int)strtol(s, NULL, 10);
}

long atol(const char* s) {
    return strtol(s, NULL, 10);
}

/* ---------------- random numbers ---------------- */

unsigned long ren_rand_next = 1;

int rand(void) {
    ren_rand_next = ren_rand_next * 1103515245UL + 12345UL;
    return (int)((ren_rand_next / 65536UL) % 32768UL);
}

void srand(unsigned int seed) {
    ren_rand_next = seed;
}

/* ---------------- sorting ---------------- */

// sorts an array (insertion sort: small, stable and fine for the sizes used in the kernel)
void qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*)) {
    unsigned char* b = (unsigned char*)base;
    for (size_t i = 1; i < count; i++) {
        for (size_t j = i; j > 0; j--) {
            unsigned char* left = b + (j - 1) * size;
            unsigned char* right = b + j * size;
            if (compare(left, right) <= 0) {
                break;
            }
            for (size_t k = 0; k < size; k++) {
                unsigned char tmp = left[k];
                left[k] = right[k];
                right[k] = tmp;
            }
        }
    }
}

/* ---------------- kernel heap ---------------- */
/*
    A simple first-fit allocator over a fixed 1MB memory pool.
    Every allocation is preceded by a small header, neighbouring free blocks are merged on free().
*/

#define HEAP_SIZE  (1024u * 1024u)
#define HEAP_ALIGN 16u
#define HEAP_MAGIC 0xC0FFEE42u

typedef struct heap_block {
    size_t size;              // usable bytes after this header
    uint32_t magic;           // HEAP_MAGIC, used to catch bad pointers passed to free()
    struct heap_block* next;  // next block in memory
    uint32_t free;            // 1 = free, 0 = in use
} heap_block_t;

uint8_t heap_area[HEAP_SIZE] __attribute__((aligned(16)));
heap_block_t* heap_head = NULL;

void heap_init(void) {
    heap_head = (heap_block_t*)heap_area;
    heap_head->size = HEAP_SIZE - sizeof(heap_block_t);
    heap_head->magic = HEAP_MAGIC;
    heap_head->next = NULL;
    heap_head->free = 1;
}

void* malloc(size_t size) {
    if (size == 0 || size > HEAP_SIZE) {
        return NULL;
    }
    if (heap_head == NULL) {
        heap_init();
    }
    size = (size + HEAP_ALIGN - 1) & ~(size_t)(HEAP_ALIGN - 1);

    for (heap_block_t* block = heap_head; block != NULL; block = block->next) {
        if (!block->free || block->size < size) {
            continue;
        }
        // split the block if the remainder is big enough to be useful
        if (block->size >= size + sizeof(heap_block_t) + HEAP_ALIGN) {
            heap_block_t* rest = (heap_block_t*)((uint8_t*)(block + 1) + size);
            rest->size = block->size - size - sizeof(heap_block_t);
            rest->magic = HEAP_MAGIC;
            rest->next = block->next;
            rest->free = 1;
            block->size = size;
            block->next = rest;
        }
        block->free = 0;
        return (void*)(block + 1);
    }
    return NULL; // out of memory
}

void free(void* ptr) {
    if (ptr == NULL) {
        return;
    }
    heap_block_t* block = (heap_block_t*)ptr - 1;
    if (block->magic != HEAP_MAGIC || block->free) {
        return; // not from malloc, or freed twice
    }
    block->free = 1;

    // merge neighbouring free blocks
    for (heap_block_t* b = heap_head; b != NULL && b->next != NULL;) {
        if (b->free && b->next->free) {
            b->size += sizeof(heap_block_t) + b->next->size;
            b->next = b->next->next;
        } else {
            b = b->next;
        }
    }
}

void* calloc(size_t count, size_t size) {
    if (size != 0 && count > (size_t)-1 / size) {
        return NULL; // overflow
    }
    void* ptr = malloc(count * size);
    if (ptr) {
        memset(ptr, 0, count * size);
    }
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    heap_block_t* block = (heap_block_t*)ptr - 1;
    if (block->magic != HEAP_MAGIC) {
        return NULL;
    }
    if (block->size >= size) {
        return ptr;
    }
    void* bigger = malloc(size);
    if (bigger == NULL) {
        return NULL;
    }
    memcpy(bigger, ptr, block->size);
    free(ptr);
    return bigger;
}

char* strdup(const char* s) {
    size_t len = strlen(s) + 1;
    char* copy = (char*)malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

char* strndup(const char* s, size_t n) {
    size_t len = strnlen(s, n);
    char* copy = (char*)malloc(len + 1);
    if (copy) {
        memcpy(copy, s, len);
        copy[len] = '\0';
    }
    return copy;
}

// heap statistics (in bytes)
size_t heap_total(void) {
    return HEAP_SIZE;
}

size_t heap_used(void) {
    if (heap_head == NULL) {
        return 0;
    }
    size_t used = 0;
    for (heap_block_t* b = heap_head; b != NULL; b = b->next) {
        if (!b->free) {
            used += b->size + sizeof(heap_block_t);
        }
    }
    return used;
}

#endif /* REN_STDLIB_H */

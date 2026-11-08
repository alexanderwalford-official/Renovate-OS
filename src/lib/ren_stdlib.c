/*
    ren_stdlib.c
    General utilities (freestanding replacement for <stdlib.h>):
    number conversion, random numbers, sorting and a kernel heap (malloc / free).

    The file is in four parts, top to bottom:
    1. integers   abs, utoa / itoa (number to text), strtol / strtoul / atoi (text to number)
    2. random     rand / srand, the tiny generator from the C standard itself
    3. sorting    qsort (really an insertion sort, see below)
    4. the heap   malloc / free / calloc / realloc / strdup over a fixed 4MB pool

    Two things worth knowing up front:
    - We never use 64 bit division here. On 32 bit x86, dividing a 64 bit number makes GCC
      call helper functions (__udivdi3 and friends) that live in libgcc, and we don't link
      libgcc. Everything in this file sticks to 32 bit maths (long is 32 bits on i386).
      ren_stdio.c shows how printf gets around this for 64 bit numbers.
    - Nothing here is protected against being interrupted half way through. That's fine
      while only normal kernel code calls these, but an interrupt handler must not call
      malloc / free / rand while the main code might be in the middle of one.

    Handy references:
        https://wiki.osdev.org/C_Library
        https://wiki.osdev.org/Memory_Allocation
        https://wiki.osdev.org/Libgcc
*/
#include "ren_stdlib.h"
#include <stdbool.h>
#include <stdint.h>
#include "ren_ctype.h"
#include "ren_string.h"

// ---------------------------------------------------------------------------------------
// integers
// ---------------------------------------------------------------------------------------

// absolute value
// gotcha: INT_MIN (-2147483648) has no positive twin in 32 bits, so abs(INT_MIN) stays negative
int abs(int value) {
    return value < 0 ? -value : value;
}

// absolute value of a long (the same size as int on 32 bit x86, kept for the standard name)
long labs(long value) {
    return value < 0 ? -value : value;
}

// convert an unsigned int to a string in any base from 2 to 36
// str must have room for the digits plus the '\0': 11 bytes covers any base 10 value,
// 33 bytes covers the worst case (base 2, 32 digits). Letters are lower case, so 255 in hex is "ff".
// returns str; an invalid base gives an empty string
// e.g. utoa(10, buf, 2) gives "1010", utoa(48879, buf, 16) gives "beef"
char* utoa(unsigned int value, char* str, int base) {
    if (base < 2 || base > 36) {
        *str = '\0';
        return str;
    }
    char* p = str;
    // the classic method: value % base is the LAST digit, value / base drops that digit,
    // repeat until nothing is left. That gives the digits backwards ("beef" comes out as "feeb"),
    // so we flip them round with strrev at the end.
    // do/while (not while) so that a value of 0 still writes a single "0"
    do {
        // index straight into a string literal: digit 0..9 -> '0'..'9', 10..35 -> 'a'..'z'
        *p++ = "0123456789abcdefghijklmnopqrstuvwxyz"[value % (unsigned int)base];
        value /= (unsigned int)base;
    } while (value);
    *p = '\0';
    return strrev(str);
}

// convert int to string
// negative numbers get a '-' in base 10, other bases show the two's complement bits
// e.g. itoa(-42, buf, 10) gives "-42", but itoa(-1, buf, 16) gives "ffffffff"
// (that's also what a programmer usually wants to see when printing a negative number in hex)
// str needs 12 bytes for any base 10 value ("-2147483648" plus the '\0')
char* itoa(int value, char* str, int base) {
    if (value < 0 && base == 10) {
        *str = '-';
        // why not just -value? INT_MIN is -2147483648 but the biggest int is 2147483647, so
        // -INT_MIN overflows (undefined behaviour). Unsigned maths wraps round instead, so
        // -(unsigned)-2147483648 is exactly 2147483648u. That gives the right digits for every negative.
        utoa(-(unsigned int)value, str + 1, 10); // negating as unsigned also works for INT_MIN
        return str;
    }
    // positive numbers (and every non base 10 number) just reuse the bits as unsigned
    return utoa((unsigned int)value, str, base);
}

// shared parser for strtol / strtoul
// reads an optional sign and the digits, and does all the work apart from deciding what to
// return. The two flags tell the callers what happened:
//   *negative = a '-' was in front (the returned value is still the positive magnitude)
//   *overflow = the digits didn't fit in an unsigned long
// *endptr, if endptr isn't NULL, is set to the first character after the number
// (or back to nptr if there was no number at all, so the caller can tell "0" apart from "abc")
static unsigned long strtoul_core(const char* nptr, char** endptr, int base, bool* negative, bool* overflow) {
    const char* s = nptr;
    unsigned long acc = 0;   // the value so far ("accumulator")
    bool any = false;        // did we see at least one valid digit?

    *negative = false;
    *overflow = false;

    // 1. leading whitespace is skipped, so "  42" parses fine
    while (isspace((unsigned char)*s)) {
        s++;
    }
    // 2. an optional sign
    if (*s == '-' || *s == '+') {
        *negative = (*s == '-');
        s++;
    }
    // 3. the base prefix: "0x" or "0X" means hex when the base is 0 (auto) or already 16
    // we only take the "0x" if a hex digit really follows, so "0x" on its own or "0xyz" parses
    // as just the "0", leaving endptr pointing at the 'x' (the same as the standard library)
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && isxdigit((unsigned char)s[2])) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        // auto base without "0x": a leading 0 means octal (C style, "0755"), anything else is decimal
        base = (s[0] == '0') ? 8 : 10;
    }
    // bases above 36 would run out of letters to use as digits
    if (base < 2 || base > 36) {
        if (endptr) {
            *endptr = (char*)nptr;
        }
        return 0;
    }

    // 4. the digits
    // overflow check: acc * base + digit must not go past ULONG_MAX. We can't just do the sum and
    // look afterwards, because by then the number has already wrapped round and looks small again.
    // So we check BEFORE: it's safe as long as acc is below cutoff, or equal to cutoff with a
    // digit no bigger than cutlim. For base 10 with 32 bit longs that's cutoff = 429496729 and
    // cutlim = 5, because ULONG_MAX is 4294967295.
    unsigned long cutoff = ULONG_MAX / (unsigned long)base;
    unsigned long cutlim = ULONG_MAX % (unsigned long)base;
    for (;; s++) {
        int c = (unsigned char)*s;
        int digit;
        // turn the character into its digit value: '0'..'9' are 0..9, letters are 10..35
        if (isdigit(c)) {
            digit = c - '0';
        } else if (isalpha(c)) {
            digit = tolower(c) - 'a' + 10;
        } else {
            break;
        }
        // e.g. '9' in octal or 'g' in hex: not part of the number, stop here
        if (digit >= base) {
            break;
        }
        any = true;
        // keep consuming digits after an overflow so endptr still lands after the number
        if (acc > cutoff || (acc == cutoff && (unsigned long)digit > cutlim)) {
            *overflow = true;
        } else {
            // shift what we have one digit to the left and add the new one
            acc = acc * (unsigned long)base + (unsigned long)digit;
        }
    }
    if (endptr) {
        *endptr = (char*)(any ? s : nptr);
    }
    return acc;
}

// parse a signed number, e.g. strtol("-42", NULL, 10) is -42, strtol("0x1F", NULL, 0) is 31
// out of range values are clamped to LONG_MIN / LONG_MAX (there's no errno to report it)
long strtol(const char* nptr, char** endptr, int base) {
    bool negative, overflow;
    unsigned long value = strtoul_core(nptr, endptr, base, &negative, &overflow);
    if (negative) {
        // the negative side reaches one further than the positive side: -2147483648 is fine,
        // even though +2147483648 isn't. So the limit for a magnitude here is LONG_MAX + 1, and
        // that exact value IS LONG_MIN (which also saves us negating something that won't fit)
        if (overflow || value >= (unsigned long)LONG_MAX + 1UL) {
            return LONG_MIN;
        }
        // value is now at most LONG_MAX, so converting to long before negating is safe
        return -(long)value;
    }
    if (overflow || value > (unsigned long)LONG_MAX) {
        return LONG_MAX;
    }
    return (long)value;
}

// parse an unsigned number, clamped to ULONG_MAX if it's too big
// a leading '-' is allowed and wraps round like the standard says, so strtoul("-1", ...) is ULONG_MAX
unsigned long strtoul(const char* nptr, char** endptr, int base) {
    bool negative, overflow;
    unsigned long value = strtoul_core(nptr, endptr, base, &negative, &overflow);
    if (overflow) {
        return ULONG_MAX;
    }
    // unsigned negation is well defined: -value is 2^32 - value
    return negative ? -value : value;
}

// text to int, base 10, with no way to tell an error apart from a real 0
// use strtol with an endptr if you need to know whether the text was really a number
int atoi(const char* s) {
    return (int)strtol(s, NULL, 10);
}

// text to long, base 10, same caveats as atoi
long atol(const char* s) {
    return strtol(s, NULL, 10);
}

// ---------------------------------------------------------------------------------------
// random numbers
// ---------------------------------------------------------------------------------------

// the generator's state; it starts at 1 so every boot gives the same sequence until srand is called
// (seeding from something that changes, like the timer tick count, gives a different one each time)
static unsigned long rand_next = 1;

// next pseudo random number from 0 to RAND_MAX (32767)
// this is a "linear congruential generator", the exact one the C standard gives as its example:
//     next = next * 1103515245 + 12345
// the "mod 2^32" part of the formula comes for free, because a 32 bit unsigned long just wraps
// round when it overflows. The low bits of an LCG are poor (the bottom bit simply alternates
// 0, 1, 0, 1...), so we throw away the bottom 16 bits (divide by 65536) and keep the next 15
// (% 32768). It's quick and fine for games, but completely predictable, never use it for security.
// see https://en.wikipedia.org/wiki/Linear_congruential_generator
int rand(void) {
    rand_next = rand_next * 1103515245UL + 12345UL;
    return (int)((rand_next / 65536UL) % 32768UL);
}

// set the generator's starting point; the same seed always replays the same sequence
void srand(unsigned int seed) {
    rand_next = seed;
}

// ---------------------------------------------------------------------------------------
// sorting
// ---------------------------------------------------------------------------------------

// sorts an array (insertion sort: small, stable and fine for the sizes used in the kernel)
// base    = the start of the array
// count   = how many elements it has
// size    = the size of ONE element in bytes (e.g. sizeof(array[0]))
// compare = your function, given pointers to two elements; return <0 if the first belongs
//           before the second, >0 if after, 0 if they're equal
// The name is the standard one so callers look normal, but it is not a quicksort. Insertion
// sort is O(n^2) in the worst case, which is nothing for a few dozen processes, and it needs no
// extra memory or recursion. "Stable" means equal elements keep their original order.
// How it works: think of sorting a hand of cards. Element i is picked up and slid left past every
// bigger element until it meets one that's not bigger. Everything to the left of i is always sorted.
void qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*)) {
    // we don't know the element type, so treat the array as raw bytes and step by size
    unsigned char* b = base;
    for (size_t i = 1; i < count; i++) {
        // slide element i left one place at a time
        for (size_t j = i; j > 0; j--) {
            unsigned char* left = b + (j - 1) * size;
            unsigned char* right = b + j * size;
            // already in order (or equal, which is what keeps the sort stable), so it has found its spot
            if (compare(left, right) <= 0) {
                break;
            }
            // swap the two elements byte by byte, which works for any element size
            // without needing a temporary buffer (and so without needing malloc)
            for (size_t k = 0; k < size; k++) {
                unsigned char tmp = left[k];
                left[k] = right[k];
                right[k] = tmp;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------
// the kernel heap
// ---------------------------------------------------------------------------------------

/*
    A simple first-fit allocator over a fixed 4MB memory pool.
    Every allocation is preceded by a small header, neighbouring free blocks are merged on free().

    Where the memory comes from:
    heap_area is just a big static array, so it lives in the kernel's .bss section, which GRUB
    clears to zero when it loads us. There is no paging or physical memory manager involved:
    the heap is always exactly these 4MB and can never grow.

    How it is laid out:
    The pool is chopped into blocks that sit one after another in memory. Each block starts
    with a heap_block_t header, and the bytes after the header are what malloc hands out.
    The headers form a linked list in address order, so "the next block in the list" is
    always "the block right after this one in memory":

        heap_area
        +--------+-----------+--------+-----------+--------+---------------------------+
        | header | 32 bytes  | header | 16 bytes  | header | the rest (free)           |
        | in use |           | free   |           | free?  |                           |
        +--------+-----------+--------+-----------+--------+---------------------------+
            |       ^ malloc     |                    ^
            +--next-+------------+--next--------------+         last block: next = NULL
                    returns this

    malloc  walks the list from the start and takes the FIRST free block big enough ("first fit").
            If that block is much bigger than needed, it is SPLIT: the front part is handed out
            and a new header is written just after it to make the leftover its own free block.
    free    marks the block free again, then MERGES (or "coalesces") any free blocks that sit
            next to each other into one big block, so the pool doesn't break up into lots of
            little pieces that are each too small to use.

    Alignment: the header is 16 bytes on 32 bit x86 (four 4 byte fields), the pool starts on a
    16 byte boundary, and every size is rounded up to a multiple of 16. So every pointer malloc
    returns is 16 byte aligned, which is safe for any type.

    See https://wiki.osdev.org/Memory_Allocation for other ways to write an allocator.
*/

// total size of the pool: 4MB
#define HEAP_SIZE  (4u * 1024u * 1024u)
// every allocation is rounded up to a multiple of this (must be a power of two for the mask trick in malloc)
#define HEAP_ALIGN 16u
// a recognisable number written into every header ("coffee"), so free() can tell a real
// block from a random pointer
#define HEAP_MAGIC 0xC0FFEE42u

// the header in front of every block, free or in use
typedef struct heap_block {
    size_t size;              // usable bytes after this header
    uint32_t magic;           // HEAP_MAGIC, used to catch bad pointers passed to free()
    struct heap_block* next;  // next block in memory
    uint32_t free;            // 1 = free, 0 = in use
} heap_block_t;

// the pool itself (in .bss, so it costs nothing in the kernel file on disk)
static uint8_t heap_area[HEAP_SIZE] __attribute__((aligned(16)));
// the first block in the list; NULL until the first malloc sets the heap up
static heap_block_t* heap_head;

// set the heap up as one big free block covering the whole pool
// called automatically by the first malloc, so nobody has to remember to initialise it
static void heap_init(void) {
    // the very first header sits at the start of the pool
    heap_head = (heap_block_t*)heap_area;
    // everything after that header is free space
    heap_head->size = HEAP_SIZE - sizeof(heap_block_t);
    heap_head->magic = HEAP_MAGIC;
    heap_head->next = NULL;
    heap_head->free = 1;
}

// allocate size bytes, returns a 16 byte aligned pointer or NULL (size 0, too big, or out of memory)
// the memory is NOT cleared, it may still hold whatever the last owner left there (use calloc for zeros)
void* malloc(size_t size) {
    // the size check also stops the rounding below from overflowing
    if (size == 0 || size > HEAP_SIZE) {
        return NULL;
    }
    // lazy initialisation on the very first call
    if (heap_head == NULL) {
        heap_init();
    }
    // round size up to the next multiple of 16: adding 15 pushes any partial chunk over the
    // next boundary, then clearing the bottom 4 bits drops it back down onto it
    // e.g. 1 -> 16, 16 -> 16, 17 -> 32
    size = (size + HEAP_ALIGN - 1) & ~(size_t)(HEAP_ALIGN - 1);

    // first fit: take the first free block that is big enough
    for (heap_block_t* block = heap_head; block != NULL; block = block->next) {
        if (!block->free || block->size < size) {
            continue;
        }
        // split the block if the remainder is big enough to be useful
        // (it has to hold a new header plus at least one 16 byte chunk, otherwise we just hand
        // out the whole block and the few spare bytes ride along with it)
        if (block->size >= size + sizeof(heap_block_t) + HEAP_ALIGN) {
            // block + 1 is pointer maths on heap_block_t, so it means "just past the header",
            // which is where the caller's data starts. The new header goes right after their data.
            heap_block_t* rest = (heap_block_t*)((uint8_t*)(block + 1) + size);
            // the leftover is what remains once our data and the new header are taken out
            rest->size = block->size - size - sizeof(heap_block_t);
            rest->magic = HEAP_MAGIC;
            // slot the new block into the list between this block and the one after it
            rest->next = block->next;
            rest->free = 1;
            block->size = size;
            block->next = rest;
        }
        block->free = 0;
        // hand out the memory just after the header
        return block + 1;
    }
    return NULL; // out of memory
}

// give a block back to the heap
// passing NULL is allowed and does nothing (like the standard). Pointers that didn't come from
// malloc, and blocks that are already free, are quietly ignored rather than corrupting the list.
void free(void* ptr) {
    if (ptr == NULL) {
        return;
    }
    // the header sits immediately before the pointer malloc handed out
    heap_block_t* block = (heap_block_t*)ptr - 1;
    // a random pointer is very unlikely to have HEAP_MAGIC just in front of it, so this catches
    // most bad pointers. It can't catch everything (e.g. a pointer into the middle of a block).
    if (block->magic != HEAP_MAGIC || block->free) {
        return; // not from malloc, or freed twice
    }
    block->free = 1;

    // merge neighbouring free blocks
    // because the list is in address order, a block and its next are touching in memory, so merging
    // just means the first block swallows the second one's header and data:
    //     [hdr A | data A][hdr B | data B]   ->   [hdr A | data A ............. ]
    // We walk the whole list (not just around this block), which keeps the code simple.
    for (heap_block_t* b = heap_head; b != NULL && b->next != NULL;) {
        if (b->free && b->next->free) {
            b->size += sizeof(heap_block_t) + b->next->size;
            b->next = b->next->next;
            // don't move on: the block after the one we swallowed might be free too,
            // so a run of three or more free blocks all collapse into one
        } else {
            b = b->next;
        }
    }
}

// allocate an array of count elements of size bytes each, all set to zero
// returns NULL if count * size would overflow (e.g. a huge count wrapping round to a small
// number and handing back far too little memory), or if there's no room
void* calloc(size_t count, size_t size) {
    // count * size > SIZE_MAX is the overflow we want to catch, but we can't compute it without
    // overflowing, so divide instead: count > SIZE_MAX / size means the same thing
    if (size != 0 && count > SIZE_MAX / size) {
        return NULL; // overflow
    }
    void* ptr = malloc(count * size);
    if (ptr) {
        memset(ptr, 0, count * size);
    }
    return ptr;
}

// change the size of an allocation, returning the (possibly new) pointer
// always use the returned pointer afterwards, the old one may have been freed. If it fails it returns
// NULL and the old block is left exactly as it was, so don't write "p = realloc(p, n)" without keeping a copy.
void* realloc(void* ptr, size_t size) {
    // the standard's special cases: no old block means plain malloc, size 0 means free
    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    heap_block_t* block = (heap_block_t*)ptr - 1;
    // not a heap block, we can't know how big it is so we can't safely copy it
    if (block->magic != HEAP_MAGIC) {
        return NULL;
    }
    // the block is already big enough (thanks to rounding it often has a few bytes spare)
    // shrinking never actually gives memory back, the block just keeps its old size
    if (block->size >= size) {
        return ptr;
    }
    // otherwise get a new, bigger block, copy the old contents over and free the old one
    void* bigger = malloc(size);
    if (bigger == NULL) {
        return NULL;
    }
    memcpy(bigger, ptr, block->size);
    free(ptr);
    return bigger;
}

// make a heap copy of s (including its '\0'), or NULL if there's no room; free() it when done
char* strdup(const char* s) {
    // + 1 for the terminator
    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

// make a heap copy of at most the first n characters of s, always terminated; free() it when done
// handy for grabbing part of a string, e.g. strndup(path, 3) on "abcdef" gives "abc"
char* strndup(const char* s, size_t n) {
    // strnlen so we never read more than n characters, even if s is longer (or unterminated)
    size_t len = strnlen(s, n);
    char* copy = malloc(len + 1);
    if (copy) {
        memcpy(copy, s, len);
        copy[len] = '\0';
    }
    return copy;
}

// the size of the whole pool in bytes (headers and free space included), shown at boot
size_t heap_total(void) {
    return HEAP_SIZE;
}

// how many bytes are currently in use, counting each used block's header as well as its data
// (so this is "how much of heap_total is taken", which the terminal shows to the user)
// it walks the whole block list, so it's O(number of blocks); returns 0 before the first malloc
size_t heap_used(void) {
    size_t used = 0;
    for (heap_block_t* b = heap_head; b != NULL; b = b->next) {
        if (!b->free) {
            used += b->size + sizeof(heap_block_t);
        }
    }
    return used;
}

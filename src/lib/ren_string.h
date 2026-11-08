/*
    ren_string.h
    String and memory functions (freestanding replacement for <string.h>).

    NOTE: GCC may emit calls to memcpy / memset / memmove / memcmp by itself (for example
    when copying structs), so these must exist with exactly these names and signatures.
*/
#ifndef REN_STRING_H
#define REN_STRING_H

#include <stddef.h>

// memory
void* memset(void* dest, int value, size_t count);
void* memcpy(void* restrict dest, const void* restrict src, size_t count);
void* memmove(void* dest, const void* src, size_t count); // safe when the regions overlap
int memcmp(const void* a, const void* b, size_t count);
void* memchr(const void* s, int c, size_t count);

// length
size_t strlen(const char* s);
size_t strnlen(const char* s, size_t max);

// comparison
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
int strcasecmp(const char* a, const char* b);
int strncasecmp(const char* a, const char* b, size_t n);

// copying
char* strcpy(char* restrict dest, const char* restrict src);
char* strncpy(char* restrict dest, const char* restrict src, size_t n);
size_t strlcpy(char* restrict dest, const char* restrict src, size_t size);
char* strcat(char* restrict dest, const char* restrict src);
char* strncat(char* restrict dest, const char* restrict src, size_t n);
size_t strlcat(char* restrict dest, const char* restrict src, size_t size);

// searching
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
char* strpbrk(const char* s, const char* accept);
char* strtok_r(char* str, const char* delim, char** saveptr);
char* strtok(char* str, const char* delim);

// non-standard extras, all work in place
char* strrev(char* s);
char* strupr(char* s);
char* strlwr(char* s);

#endif /* REN_STRING_H */

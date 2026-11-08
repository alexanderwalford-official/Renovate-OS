/*
    ren_string.c
    String and memory functions (freestanding replacement for <string.h>).
*/
#include "ren_string.h"
#include "ren_ctype.h"

void* memset(void* dest, int value, size_t count) {
    unsigned char* d = dest;
    while (count--) {
        *d++ = (unsigned char)value;
    }
    return dest;
}

void* memcpy(void* restrict dest, const void* restrict src, size_t count) {
    unsigned char* d = dest;
    const unsigned char* s = src;
    while (count--) {
        *d++ = *s++;
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t count) {
    unsigned char* d = dest;
    const unsigned char* s = src;
    if (d < s) {
        while (count--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        // copy backwards so the end of src is read before it is overwritten
        d += count;
        s += count;
        while (count--) {
            *--d = *--s;
        }
    }
    return dest;
}

int memcmp(const void* a, const void* b, size_t count) {
    const unsigned char* x = a;
    const unsigned char* y = b;
    for (; count; --count, ++x, ++y) {
        if (*x != *y) {
            return *x - *y;
        }
    }
    return 0;
}

void* memchr(const void* s, int c, size_t count) {
    const unsigned char* p = s;
    for (; count; --count, ++p) {
        if (*p == (unsigned char)c) {
            return (void*)p;
        }
    }
    return NULL;
}

size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

size_t strnlen(const char* s, size_t max) {
    size_t len = 0;
    while (len < max && s[len] != '\0') {
        len++;
    }
    return len;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (; n; --n, ++a, ++b) {
        if (*a != *b || *a == '\0') {
            return (unsigned char)*a - (unsigned char)*b;
        }
    }
    return 0;
}

// case-insensitive compare (POSIX)
int strcasecmp(const char* a, const char* b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char* a, const char* b, size_t n) {
    for (; n; --n, ++a, ++b) {
        int x = tolower((unsigned char)*a);
        int y = tolower((unsigned char)*b);
        if (x != y || x == '\0') {
            return x - y;
        }
    }
    return 0;
}

char* strcpy(char* restrict dest, const char* restrict src) {
    char* d = dest;
    while ((*d++ = *src++) != '\0')
        ;
    return dest;
}

// copies at most n characters, pads with '\0' (and does NOT terminate if src is too long, like the standard)
char* strncpy(char* restrict dest, const char* restrict src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

// safe copy (BSD): always terminates, returns strlen(src) so truncation can be detected
size_t strlcpy(char* restrict dest, const char* restrict src, size_t size) {
    size_t src_len = strlen(src);
    if (size > 0) {
        size_t n = (src_len >= size) ? size - 1 : src_len;
        memcpy(dest, src, n);
        dest[n] = '\0';
    }
    return src_len;
}

char* strcat(char* restrict dest, const char* restrict src) {
    strcpy(dest + strlen(dest), src);
    return dest;
}

char* strncat(char* restrict dest, const char* restrict src, size_t n) {
    char* d = dest + strlen(dest);
    while (n-- && *src) {
        *d++ = *src++;
    }
    *d = '\0';
    return dest;
}

// safe append (BSD): always terminates, returns the length it tried to create
size_t strlcat(char* restrict dest, const char* restrict src, size_t size) {
    size_t dest_len = strnlen(dest, size);
    if (dest_len == size) {
        return size + strlen(src); // dest was not terminated, there is no room at all
    }
    return dest_len + strlcpy(dest + dest_len, src, size - dest_len);
}

char* strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) {
            return (char*)s;
        }
        if (*s == '\0') {
            return NULL;
        }
    }
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    for (;; s++) {
        if (*s == (char)c) {
            last = s;
        }
        if (*s == '\0') {
            return (char*)last;
        }
    }
}

char* strstr(const char* haystack, const char* needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return (char*)haystack;
    }
    for (; *haystack; haystack++) {
        if (*haystack == *needle && strncmp(haystack, needle, needle_len) == 0) {
            return (char*)haystack;
        }
    }
    return NULL;
}

// length of the prefix of s made only of characters in accept
size_t strspn(const char* s, const char* accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) {
        n++;
    }
    return n;
}

// length of the prefix of s made only of characters NOT in reject
size_t strcspn(const char* s, const char* reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) {
        n++;
    }
    return n;
}

char* strpbrk(const char* s, const char* accept) {
    s += strcspn(s, accept);
    return *s ? (char*)s : NULL;
}

// re-entrant tokenizer: splits str on any character in delim (modifies str)
char* strtok_r(char* str, const char* delim, char** saveptr) {
    if (str == NULL) {
        str = *saveptr;
        if (str == NULL) {
            return NULL;
        }
    }
    str += strspn(str, delim);
    if (*str == '\0') {
        *saveptr = NULL;
        return NULL;
    }
    char* end = str + strcspn(str, delim);
    if (*end != '\0') {
        *end = '\0';
        *saveptr = end + 1;
    } else {
        *saveptr = NULL;
    }
    return str;
}

char* strtok(char* str, const char* delim) {
    static char* saved;
    return strtok_r(str, delim, &saved);
}

// reverse a string in place
char* strrev(char* s) {
    size_t len = strlen(s);
    for (size_t i = 0; i < len / 2; i++) {
        char tmp = s[i];
        s[i] = s[len - 1 - i];
        s[len - 1 - i] = tmp;
    }
    return s;
}

// convert a string to upper / lower case in place
char* strupr(char* s) {
    for (char* p = s; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
    return s;
}

char* strlwr(char* s) {
    for (char* p = s; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
    return s;
}

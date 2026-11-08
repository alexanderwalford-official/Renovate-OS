/*
    aer_string.c
    String and memory functions (freestanding replacement for <string.h>).

    This is the code behind aer_string.h. Nearly everything in the kernel leans on it:
    the terminal parses commands with strcmp / split, the file systems copy names
    around with strlcpy, printf measures strings with strlen, and GCC itself quietly
    calls memcpy / memset whenever it copies or clears a struct (see aer_string.h).

    Design choices:
    - Everything works one byte at a time. A real libc would copy 4 or 8 bytes per step,
      use SSE, and worry about alignment, but simple loops are easy to read, easy to
      trust, and plenty fast for a hobby kernel. (SSE is switched off anyway, the CPU
      hasn't been set up to use it, see the -mno-sse flags in the Makefile.)
    - Bytes are compared as unsigned char. That's what the C standard asks for, and it
      means a character like 0xE9 sorts after 'z' instead of before '\0' (on x86 a plain
      char is signed, so 0xE9 would otherwise be -23).
    - None of these functions check for NULL pointers, exactly like the real ones.
      Passing NULL is a bug in the caller, and the kernel has no page protection to
      catch it, so it would quietly read whatever lives at address 0.

    Handy references:
        https://wiki.osdev.org/C_Library
        https://en.cppreference.com/w/c/string/byte
*/
#include "aer_string.h"
#include "aer_ctype.h"

// fill count bytes starting at dest with value, returns dest
// value is an int for historical reasons, only its low 8 bits are written (memset(p, 0x1FF, n) writes 0xFF)
// note: the Makefile's -fno-tree-loop-distribute-patterns flag is what stops GCC turning this
// very loop into a call to memset, which would recurse forever
void* memset(void* dest, int value, size_t count) {
    // void* can't be dereferenced, so look at the memory as plain bytes
    unsigned char* d = dest;
    // count-- tests the old value, so the loop body runs exactly count times
    while (count--) {
        *d++ = (unsigned char)value;
    }
    return dest;
}

// copy count bytes from src to dest, returns dest
// the regions must not overlap (that is what "restrict" promises), use memmove if they might
void* memcpy(void* restrict dest, const void* restrict src, size_t count) {
    unsigned char* d = dest;
    const unsigned char* s = src;
    // plain front to back copy, one byte at a time
    while (count--) {
        *d++ = *s++;
    }
    return dest;
}

// copy count bytes from src to dest even when the two regions overlap, returns dest
// the trick is choosing the copy direction so we never overwrite a source byte before reading it:
//
//     dest before src:  [d d d d]                  copy front to back
//                           [s s s s]
//
//     dest after src:       [d d d d]              copy back to front
//                       [s s s s]
//
// this is what StrRemove uses to slide the rest of a string left over the removed text
void* memmove(void* dest, const void* src, size_t count) {
    unsigned char* d = dest;
    const unsigned char* s = src;
    if (d < s) {
        // dest starts below src: by the time we write a byte, its source has already been read
        while (count--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        // copy backwards so the end of src is read before it is overwritten
        // start one past the last byte, then pre-decrement so the first byte copied is the last one
        d += count;
        s += count;
        while (count--) {
            *--d = *--s;
        }
    }
    // (if d == s the regions are the same memory and there is nothing to do)
    return dest;
}

// compare the first count bytes of a and b
// returns 0 if they're all equal, otherwise the difference between the first pair that differs
// (so the sign tells you which one is "bigger", treating each byte as 0..255)
int memcmp(const void* a, const void* b, size_t count) {
    const unsigned char* x = a;
    const unsigned char* y = b;
    // walk both buffers together until count runs out
    for (; count; --count, ++x, ++y) {
        if (*x != *y) {
            // both values are promoted to int before subtracting, so 0 - 255 is -255, no wrap around
            return *x - *y;
        }
    }
    return 0;
}

// look for the byte c in the first count bytes of s
// returns a pointer to the first match, or NULL if it isn't there
// unlike strchr this doesn't care about '\0', it always scans the full count bytes
void* memchr(const void* s, int c, size_t count) {
    const unsigned char* p = s;
    for (; count; --count, ++p) {
        // c is an int (like memset), compare against its low byte only
        if (*p == (unsigned char)c) {
            // cast away const: the standard signature returns a plain void* for convenience
            return (void*)p;
        }
    }
    return NULL;
}

// the length of a string: how many characters come before the '\0'
// strlen("abc") is 3, strlen("") is 0
size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

// like strlen, but gives up after max characters
// useful for fixed size buffers that might not have a terminator (e.g. names read off a disk),
// or for printf's "%.5s" which must not read further than 5 characters
size_t strnlen(const char* s, size_t max) {
    size_t len = 0;
    // check len < max FIRST so we never even read the byte at s[max]
    while (len < max && s[len] != '\0') {
        len++;
    }
    return len;
}

// compare two strings character by character
// returns 0 if they are identical, <0 if a sorts before b, >0 if a sorts after b
// e.g. strcmp("cat", "car") > 0 because 't' > 'r', strcmp("car", "cart") < 0 because '\0' < 't'
int strcmp(const char* a, const char* b) {
    // skip over the matching part; stop at the end of a or at the first difference
    // (we don't need to test *b for '\0': if b ended first then *a != *b and the loop stops anyway)
    while (*a && *a == *b) {
        a++;
        b++;
    }
    // compare as unsigned so characters above 127 sort after the normal ASCII ones
    return (unsigned char)*a - (unsigned char)*b;
}

// compare at most the first n characters of two strings, same return values as strcmp
// strncmp("help me", "help", 4) == 0, handy for checking a prefix
int strncmp(const char* a, const char* b, size_t n) {
    for (; n; --n, ++a, ++b) {
        // stop at the first difference, or when both strings end together (*a == *b == '\0')
        if (*a != *b || *a == '\0') {
            return (unsigned char)*a - (unsigned char)*b;
        }
    }
    // the first n characters all matched
    return 0;
}

// case-insensitive compare (POSIX)
// works like strcmp but folds both sides to lower case first, so "LS" matches "ls"
// the terminal uses this to look up commands, and the FAT driver uses it because FAT file
// names are case-insensitive ("README.TXT" and "readme.txt" are the same file)
int strcasecmp(const char* a, const char* b) {
    // the casts to unsigned char keep tolower's argument in range (see aer_ctype.h)
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

// case-insensitive compare of at most the first n characters
int strncasecmp(const char* a, const char* b, size_t n) {
    for (; n; --n, ++a, ++b) {
        int x = tolower((unsigned char)*a);
        int y = tolower((unsigned char)*b);
        // same stopping rule as strncmp: a difference, or the end of both strings
        if (x != y || x == '\0') {
            return x - y;
        }
    }
    return 0;
}

// copy src into dest, including the terminator, and return dest
// gotcha: there is no size check, dest must be at least strlen(src) + 1 bytes (prefer strlcpy)
char* strcpy(char* restrict dest, const char* restrict src) {
    char* d = dest;
    // the classic one-liner: copy a byte, then stop once the byte we just copied was the '\0'
    // all the work happens in the condition, so the loop body is empty
    while ((*d++ = *src++) != '\0')
        ;
    return dest;
}

// copies at most n characters, pads with '\0' (and does NOT terminate if src is too long, like the standard)
// strncpy was designed for fixed width records (old directory entries), not for safe string copying,
// so in new code strlcpy is almost always what you want
char* strncpy(char* restrict dest, const char* restrict src, size_t n) {
    size_t i = 0;
    // copy the characters of src, stopping at its end or after n of them
    for (; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    // fill whatever is left of the n bytes with zeros
    // (if src had n or more characters this loop does nothing, so dest ends up unterminated)
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

// safe copy (BSD): always terminates, returns strlen(src) so truncation can be detected
// size is the full size of dest in bytes (including room for the '\0'), e.g.
//     char name[8];
//     if (strlcpy(name, input, sizeof(name)) >= sizeof(name)) { ...input was cut short... }
size_t strlcpy(char* restrict dest, const char* restrict src, size_t size) {
    size_t src_len = strlen(src);
    // with size 0 there isn't even room for a '\0', so we don't touch dest at all
    if (size > 0) {
        // copy everything if it fits, otherwise as much as fits while leaving 1 byte for the terminator
        size_t n = (src_len >= size) ? size - 1 : src_len;
        memcpy(dest, src, n);
        dest[n] = '\0';
    }
    // returning the length of src (not what was copied) lets callers spot truncation
    return src_len;
}

// append src to the end of dest, returns dest
// gotcha: no size check, dest needs room for both strings plus the '\0' (prefer strlcat)
char* strcat(char* restrict dest, const char* restrict src) {
    // find dest's terminator and copy src over it
    strcpy(dest + strlen(dest), src);
    return dest;
}

// append at most n characters of src to dest, then ALWAYS add a '\0'
// (unlike strncpy) so dest needs strlen(dest) + n + 1 bytes of room
char* strncat(char* restrict dest, const char* restrict src, size_t n) {
    // start writing where dest's terminator is now
    char* d = dest + strlen(dest);
    // stop after n characters or at the end of src, whichever comes first
    while (n-- && *src) {
        *d++ = *src++;
    }
    *d = '\0';
    return dest;
}

// safe append (BSD): always terminates, returns the length it tried to create
// size is the full size of the dest buffer, not the space that's left
// if the return value is >= size the result was cut short
size_t strlcat(char* restrict dest, const char* restrict src, size_t size) {
    // strnlen so we never read past the end of the buffer if dest isn't terminated
    size_t dest_len = strnlen(dest, size);
    if (dest_len == size) {
        return size + strlen(src); // dest was not terminated, there is no room at all
    }
    // copy src into whatever space is left after the existing text; strlcpy handles the
    // truncation and the terminator, and returns strlen(src) which gives us the full "wanted" length
    return dest_len + strlcpy(dest + dest_len, src, size - dest_len);
}

// find the first occurrence of the character c in s, or NULL
// the '\0' counts as part of the string, so strchr(s, '\0') returns a pointer to the end of s
char* strchr(const char* s, int c) {
    // an endless loop that leaves through one of the two returns
    for (;; s++) {
        // check for a match BEFORE checking for the end, that's what lets c == '\0' be found
        if (*s == (char)c) {
            return (char*)s;
        }
        if (*s == '\0') {
            return NULL;
        }
    }
}

// find the LAST occurrence of the character c in s, or NULL
// e.g. strrchr("/sys/docs/readme", '/') points at "/readme", handy for splitting off a file name
char* strrchr(const char* s, int c) {
    const char* last = NULL;
    // walk the whole string remembering the most recent match
    for (;; s++) {
        if (*s == (char)c) {
            last = s;
        }
        if (*s == '\0') {
            return (char*)last;
        }
    }
}

// find the first place needle appears inside haystack, or NULL if it doesn't
// this is the simple "try every starting position" search: fine for short strings like commands
// and paths (worst case it's O(haystack length * needle length))
char* strstr(const char* haystack, const char* needle) {
    size_t needle_len = strlen(needle);
    // the standard says an empty needle is found at the very start
    if (needle_len == 0) {
        return (char*)haystack;
    }
    for (; *haystack; haystack++) {
        // checking the first character is a cheap filter before the full comparison
        // (strncmp also stops safely if the haystack runs out before needle_len characters)
        if (*haystack == *needle && strncmp(haystack, needle, needle_len) == 0) {
            return (char*)haystack;
        }
    }
    return NULL;
}

// length of the prefix of s made only of characters in accept
// e.g. strspn("   ls", " ") is 3, the number of leading spaces
size_t strspn(const char* s, const char* accept) {
    size_t n = 0;
    // the s[n] test matters: strchr would happily "find" the '\0' at the end of accept
    while (s[n] && strchr(accept, s[n])) {
        n++;
    }
    return n;
}

// length of the prefix of s made only of characters NOT in reject
// e.g. strcspn("ls /sys", " ") is 2, the length of the first word
size_t strcspn(const char* s, const char* reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) {
        n++;
    }
    return n;
}

// find the first character in s that is any of the characters in accept, or NULL if there are none
// e.g. strpbrk("a,b;c", ",;") points at ",b;c"
char* strpbrk(const char* s, const char* accept) {
    // skip everything that isn't in accept; we either land on a match or on the terminator
    s += strcspn(s, accept);
    return *s ? (char*)s : NULL;
}

// re-entrant tokenizer: splits str on any character in delim (modifies str)
// call it with the string the first time, then with NULL to get each next token:
//     char line[] = "  copy  a.txt b.txt";
//     char* save;
//     for (char* t = strtok_r(line, " ", &save); t; t = strtok_r(NULL, " ", &save)) { ... }
// gives "copy", "a.txt", "b.txt". Runs of delimiters are skipped, so there are no empty tokens.
// "re-entrant" means all the state lives in *saveptr, so two loops can tokenise different
// strings at the same time without getting in each other's way
char* strtok_r(char* str, const char* delim, char** saveptr) {
    // NULL means "carry on with the string from last time"
    if (str == NULL) {
        str = *saveptr;
        // the previous call hit the end of the string, nothing left to hand out
        if (str == NULL) {
            return NULL;
        }
    }
    // skip any delimiters in front of the token
    str += strspn(str, delim);
    // only delimiters were left, so there are no more tokens
    if (*str == '\0') {
        *saveptr = NULL;
        return NULL;
    }
    // the token runs until the next delimiter (or the end of the string)
    char* end = str + strcspn(str, delim);
    if (*end != '\0') {
        // chop the string here by overwriting the delimiter with a terminator,
        // and remember to carry on just after it next time
        *end = '\0';
        *saveptr = end + 1;
    } else {
        // this token runs to the end of the string, so it is the last one
        *saveptr = NULL;
    }
    return str;
}

// the classic standard tokenizer, same as strtok_r but it keeps its place in a hidden static
// gotcha: because there is only one "saved" for the whole kernel, you can't tokenise two strings
// at once (e.g. a nested loop, or an interrupt handler) - use strtok_r for that
char* strtok(char* str, const char* delim) {
    // static locals live in .bss for the life of the kernel and start out as NULL
    static char* saved;
    return strtok_r(str, delim, &saved);
}

// reverse a string in place
// utoa uses this: it works out digits from the least significant end, then flips them round
char* strrev(char* s) {
    size_t len = strlen(s);
    // swap the first and last characters, then the second and second to last, and so on,
    // meeting in the middle (with an odd length the middle character stays where it is)
    for (size_t i = 0; i < len / 2; i++) {
        char tmp = s[i];
        s[i] = s[len - 1 - i];
        s[len - 1 - i] = tmp;
    }
    return s;
}

// convert a string to upper / lower case in place
// anything that isn't a letter is left alone; both return s so calls can be chained
char* strupr(char* s) {
    for (char* p = s; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
    return s;
}

// the lower case twin of strupr: "Hello World" becomes "hello world"
char* strlwr(char* s) {
    for (char* p = s; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
    return s;
}

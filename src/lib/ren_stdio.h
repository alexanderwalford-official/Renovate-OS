/*
    ren_stdio.h
    Formatted output (freestanding replacement for the output half of <stdio.h>).

    printf / putchar write to the console (scrolling terminal at the cursor position).
    sprintf / snprintf / vsnprintf write into a buffer.

    Supported: %d %i %u %x %X %o %b %c %s %p %%
               flags - 0 + space #, width, .precision (both may be *),
               length modifiers hh h l ll z t j
*/
#ifndef REN_STDIO_H
#define REN_STDIO_H

#include <stdarg.h>
#include <stddef.h>

#ifndef EOF
#define EOF (-1)
#endif

// let gcc check the format strings of every call (-Wformat)
int printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int sprintf(char* buf, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char* buf, size_t size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

int vprintf(const char* fmt, va_list ap);
int vsprintf(char* buf, const char* fmt, va_list ap);
// returns the length the full string would have had (like the standard), output is always terminated
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);

int putchar(int c);

#endif /* REN_STDIO_H */

/*
    aer_stdio.h
    Formatted output (freestanding replacement for the output half of <stdio.h>).

    prtf / putchar write to the console (scrolling terminal at the cursor position).
    sprintf / sfmt / vsnprintf write into a buffer.

    Supported: %d %i %u %x %X %o %b %c %s %p %%
               flags - 0 + space #, width, .precision (both may be *),
               length modifiers hh h l ll z t j

    A format specifier is built from up to five parts, always in this order:

        %  [flags]  [width]  [.precision]  [length]  conversion
        e.g.  %-08.3lx  =  flags "-0", width 8, precision 3, length "l", conversion 'x'

    conversions   d i    signed decimal          u     unsigned decimal
                  x X    hex (lower / upper)     o     octal
                  b      binary (not standard C, but very handy in a kernel for looking at bit flags)
                  c      one character           s     a string (NULL prints "(null)")
                  p      a pointer, as 0x...     %%    a literal '%'
    flags         -      left align inside the width (pad on the right)
                  0      pad numbers with zeros instead of spaces
                  +      always show a sign on signed numbers ("+5")
                  space  put a space where the '+' would go (" 5"), so columns line up
                  #      "alternative form": 0x for hex, 0 for octal, 0b for binary
    width         the minimum number of characters to print, padding if shorter (never truncates)
    .precision    for numbers: the minimum number of digits (zero filled)
                  for %s: the MAXIMUM number of characters to print
    *             in place of a width or precision number: take it from the next int argument
    length        hh char, h short, l long, ll long long (64 bit),
                  z size_t, t ptrdiff_t, j intmax_t

    Not supported: floating point (%f %e %g, the kernel never prints floats), %n, and
    positional arguments like %1$d. An unknown conversion is printed as it was written.

    There's no input half (scanf, getchar): keyboard input goes through the drivers instead.

    Handy references:
        https://en.cppreference.com/w/c/io/fprintf
        https://wiki.osdev.org/Printing_To_Screen
*/
#ifndef AER_STDIO_H
#define AER_STDIO_H

#include <stdarg.h>
#include <stddef.h>

// the "end of file" value from the real stdio, kept so code written against the standard still compiles
#ifndef EOF
#define EOF (-1)
#endif

// let gcc check the format strings of every call (-Wformat)
// format(printf, 1, 2) reads as "argument 1 is a printf style format string, and the values it
// describes start at argument 2". GCC then warns at compile time about things like prtf("%s", 42).

// print to the console, returns the number of characters printed.
// prtf is AerOS's name for the standard C printf: it works exactly the same way. (The
// format(printf, ...) after it must keep saying printf: that is the name GCC knows the
// format checking by, not the name of this function)
int prtf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// print into buf, returns the length written (excluding the '\0')
// gotcha: there's no size limit, so buf must be big enough. Prefer sfmt.
int sprintf(char* buf, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
// print into buf, which is size bytes long; never writes past the end and always terminates
// (when size is at least 1). Returns the length the full result needed, so ">= size" means it was cut short.
int sfmt(char* buf, size_t size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

// the "v" versions take a va_list instead of "...", so you can write your own printf style functions:
//     void debug_print(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprtf(fmt, ap); va_end(ap); }

// prtf with a va_list
int vprtf(const char* fmt, va_list ap);
// sprintf with a va_list (same "no size limit" gotcha)
int vsprintf(char* buf, const char* fmt, va_list ap);
// returns the length the full string would have had (like the standard), output is always terminated
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);

// print one character to the console and move the cursor, returns the character (as 0..255)
int putchar(int c);

#endif /* AER_STDIO_H */

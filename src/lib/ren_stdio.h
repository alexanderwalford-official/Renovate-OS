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
#include <stdint.h>
#include <stdbool.h>
#include "ren_ctype.h"
#include "ren_string.h"
#include "../drivers/video/VGA_linear.h"

#ifndef EOF
#define EOF (-1)
#endif

typedef void (*ren_fmt_out_t)(char c, void* ctx);

// let gcc check the format strings of every call (-Wformat)
int printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int sprintf(char* buf, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char* buf, size_t size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

// divide a 64 bit number by a 32 bit one without needing libgcc (__udivdi3), returns the remainder
uint32_t ren_divmod64(unsigned long long* n, uint32_t divisor) {
    uint32_t high = (uint32_t)(*n >> 32);
    uint32_t low = (uint32_t)*n;
    uint32_t q_high = high / divisor;
    uint32_t rem = high % divisor;
    uint32_t q_low;
    // rem < divisor, so (rem:low) / divisor always fits in 32 bits
    __asm__ ("divl %4" : "=a"(q_low), "=d"(rem) : "0"(low), "1"(rem), "rm"(divisor));
    *n = ((unsigned long long)q_high << 32) | q_low;
    return rem;
}

// write the digits of value into buf (reversed), returns the number of digits
int ren_fmt_digits(unsigned long long value, uint32_t base, bool upper, char* buf) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do {
        buf[n++] = digits[ren_divmod64(&value, base)];
    } while (value != 0);
    return n;
}

// the formatting engine shared by every printf style function
int ren_format(ren_fmt_out_t out, void* ctx, const char* fmt, va_list ap) {
    int count = 0;

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out(*fmt, ctx);
            count++;
            continue;
        }
        fmt++;

        // flags
        bool left = false, zero = false, plus = false, space = false, alt = false;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else if (*fmt == '+') plus = true;
            else if (*fmt == ' ') space = true;
            else if (*fmt == '#') alt = true;
            else break;
        }

        // width
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = true;
                width = -width;
            }
            fmt++;
        } else {
            while (isdigit((unsigned char)*fmt)) {
                width = width * 10 + (*fmt++ - '0');
            }
        }

        // precision
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                if (precision < 0) {
                    precision = -1;
                }
                fmt++;
            } else {
                while (isdigit((unsigned char)*fmt)) {
                    precision = precision * 10 + (*fmt++ - '0');
                }
            }
        }

        // length: -2 = hh, -1 = h, 0 = int, 1 = long, 2 = long long
        int length = 0;
        if (*fmt == 'h') {
            length = -1;
            fmt++;
            if (*fmt == 'h') { length = -2; fmt++; }
        } else if (*fmt == 'l') {
            length = 1;
            fmt++;
            if (*fmt == 'l') { length = 2; fmt++; }
        } else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') {
            length = (*fmt == 'j') ? 2 : 1;
            fmt++;
        }

        char spec = *fmt;
        if (spec == '\0') {
            break;
        }

        char digits[72];
        int ndigits = 0;
        const char* prefix = "";
        const char* str = NULL;
        int str_len = 0;
        bool is_number = false;

        switch (spec) {
            case 'd':
            case 'i': {
                long long value;
                if (length == 2) value = va_arg(ap, long long);
                else if (length == 1) value = va_arg(ap, long);
                else value = va_arg(ap, int);
                if (length == -1) value = (short)value;
                if (length == -2) value = (signed char)value;
                unsigned long long magnitude = (value < 0) ? (unsigned long long)(-(value + 1)) + 1 : (unsigned long long)value;
                if (value < 0) prefix = "-";
                else if (plus) prefix = "+";
                else if (space) prefix = " ";
                ndigits = ren_fmt_digits(magnitude, 10, false, digits);
                is_number = true;
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o':
            case 'b':
            case 'p': {
                unsigned long long value;
                uint32_t base = 10;
                if (spec == 'p') {
                    value = (uintptr_t)va_arg(ap, void*);
                    alt = true;
                } else if (length == 2) value = va_arg(ap, unsigned long long);
                else if (length == 1) value = va_arg(ap, unsigned long);
                else value = va_arg(ap, unsigned int);
                if (length == -1) value = (unsigned short)value;
                if (length == -2) value = (unsigned char)value;

                if (spec == 'x' || spec == 'p') base = 16;
                else if (spec == 'X') base = 16;
                else if (spec == 'o') base = 8;
                else if (spec == 'b') base = 2;

                if (alt && value != 0) {
                    if (base == 16) prefix = (spec == 'X') ? "0X" : "0x";
                    else if (base == 8) prefix = "0";
                    else if (base == 2) prefix = "0b";
                }
                ndigits = ren_fmt_digits(value, base, spec == 'X', digits);
                is_number = true;
                break;
            }
            case 'c':
                digits[0] = (char)va_arg(ap, int);
                str = digits;
                str_len = 1;
                break;
            case 's':
                str = va_arg(ap, const char*);
                if (str == NULL) {
                    str = "(null)";
                }
                str_len = (precision >= 0) ? (int)strnlen(str, (size_t)precision) : (int)strlen(str);
                break;
            case '%':
                str = "%";
                str_len = 1;
                break;
            default:
                // unknown conversion, print it as-is
                out('%', ctx);
                out(spec, ctx);
                count += 2;
                continue;
        }

        if (is_number) {
            // precision = minimum number of digits (and ".0" with value 0 prints nothing)
            if (precision == 0 && ndigits == 1 && digits[0] == '0') {
                ndigits = 0;
            }
            int zeros = (precision > ndigits) ? precision - ndigits : 0;
            int prefix_len = (int)strlen(prefix);
            int total = prefix_len + zeros + ndigits;
            int pad = (width > total) ? width - total : 0;

            if (zero && !left && precision < 0) {
                zeros += pad;
                pad = 0;
            }
            if (!left) {
                for (int i = 0; i < pad; i++) { out(' ', ctx); count++; }
            }
            for (int i = 0; i < prefix_len; i++) { out(prefix[i], ctx); count++; }
            for (int i = 0; i < zeros; i++) { out('0', ctx); count++; }
            for (int i = ndigits - 1; i >= 0; i--) { out(digits[i], ctx); count++; }
            if (left) {
                for (int i = 0; i < pad; i++) { out(' ', ctx); count++; }
            }
        } else {
            int pad = (width > str_len) ? width - str_len : 0;
            if (!left) {
                for (int i = 0; i < pad; i++) { out(' ', ctx); count++; }
            }
            for (int i = 0; i < str_len; i++) { out(str[i], ctx); count++; }
            if (left) {
                for (int i = 0; i < pad; i++) { out(' ', ctx); count++; }
            }
        }
    }
    return count;
}

/* ---------------- buffer output ---------------- */

typedef struct {
    char* buf;
    size_t size;
    size_t pos;
} ren_buffer_ctx_t;

void ren_buffer_out(char c, void* ctx) {
    ren_buffer_ctx_t* b = (ren_buffer_ctx_t*)ctx;
    if (b->size > 0 && b->pos < b->size - 1) {
        b->buf[b->pos] = c;
    }
    b->pos++;
}

// returns the length the full string would have had (like the standard), output is always terminated
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
    ren_buffer_ctx_t ctx = { buf, size, 0 };
    int n = ren_format(ren_buffer_out, &ctx, fmt, ap);
    if (size > 0) {
        buf[(ctx.pos < size) ? ctx.pos : size - 1] = '\0';
    }
    return n;
}

int snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int vsprintf(char* buf, const char* fmt, va_list ap) {
    return vsnprintf(buf, (size_t)INT32_MAX, fmt, ap);
}

int sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

/* ---------------- console output ---------------- */

void ren_console_out(char c, void* ctx) {
    (void)ctx;
    console_putchar_raw(c);
}

int vprintf(const char* fmt, va_list ap) {
    int n = ren_format(ren_console_out, NULL, fmt, ap);
    console_update_cursor();
    return n;
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int putchar(int c) {
    console_putchar((char)c);
    return (unsigned char)c;
}

#endif /* REN_STDIO_H */

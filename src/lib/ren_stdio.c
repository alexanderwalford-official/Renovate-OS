/*
    ren_stdio.c
    Formatted output (freestanding replacement for the output half of <stdio.h>).
    Every printf style function feeds the same formatting engine, only the place
    the characters end up (console or buffer) differs.
*/
#include "ren_stdio.h"
#include <stdbool.h>
#include <stdint.h>
#include "ren_ctype.h"
#include "ren_string.h"
#include "../drivers/video/VGA_linear.h"

typedef void (*out_fn)(char c, void* ctx);

// divide a 64 bit number by a 32 bit one without needing libgcc (__udivdi3), returns the remainder
static uint32_t divmod64(unsigned long long* n, uint32_t divisor) {
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

// write the digits of value so they end just before end, returns where they start
static char* format_digits(unsigned long long value, uint32_t base, bool upper, char* end) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        *--end = digits[divmod64(&value, base)];
    } while (value != 0);
    return end;
}

static void emit_repeat(out_fn out, void* ctx, char c, int n) {
    for (int i = 0; i < n; i++) {
        out(c, ctx);
    }
}

static void emit_chars(out_fn out, void* ctx, const char* s, int n) {
    for (int i = 0; i < n; i++) {
        out(s[i], ctx);
    }
}

// the formatting engine shared by every printf style function
static int vformat(out_fn out, void* ctx, const char* fmt, va_list ap) {
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

        // width, a negative * width means left aligned
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

        // precision, -1 when not given
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

        char buf[64]; // enough for a 64 bit number in binary
        char* buf_end = buf + sizeof(buf);
        const char* body = NULL;
        int len = 0;
        const char* prefix = "";
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

                if (value < 0) prefix = "-";
                else if (plus) prefix = "+";
                else if (space) prefix = " ";
                // negate as unsigned so the most negative value works too
                unsigned long long magnitude = (value < 0) ? -(unsigned long long)value : (unsigned long long)value;
                body = format_digits(magnitude, 10, false, buf_end);
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
                if (spec == 'p') {
                    value = (uintptr_t)va_arg(ap, void*);
                    alt = true;
                } else if (length == 2) value = va_arg(ap, unsigned long long);
                else if (length == 1) value = va_arg(ap, unsigned long);
                else value = va_arg(ap, unsigned int);
                if (length == -1) value = (unsigned short)value;
                if (length == -2) value = (unsigned char)value;

                uint32_t base = 10;
                if (spec == 'x' || spec == 'X' || spec == 'p') base = 16;
                else if (spec == 'o') base = 8;
                else if (spec == 'b') base = 2;

                if (alt && value != 0) {
                    if (base == 16) prefix = (spec == 'X') ? "0X" : "0x";
                    else if (base == 8) prefix = "0";
                    else if (base == 2) prefix = "0b";
                }
                body = format_digits(value, base, spec == 'X', buf_end);
                is_number = true;
                break;
            }
            case 'c':
                buf[0] = (char)va_arg(ap, int);
                body = buf;
                len = 1;
                break;
            case 's':
                body = va_arg(ap, const char*);
                if (body == NULL) {
                    body = "(null)";
                }
                len = (precision >= 0) ? (int)strnlen(body, (size_t)precision) : (int)strlen(body);
                break;
            case '%':
                body = "%";
                len = 1;
                break;
            default:
                // unknown conversion, print it as-is
                out('%', ctx);
                out(spec, ctx);
                count += 2;
                continue;
        }

        int zeros = 0;
        if (is_number) {
            len = (int)(buf_end - body);
            // precision = minimum number of digits (and ".0" with value 0 prints nothing)
            if (precision == 0 && len == 1 && body[0] == '0') {
                len = 0;
            }
            if (precision > len) {
                zeros = precision - len;
            }
        }
        int prefix_len = (int)strlen(prefix);
        int pad = width - prefix_len - zeros - len;
        if (pad < 0) {
            pad = 0;
        }
        if (is_number && zero && !left && precision < 0) {
            zeros += pad;
            pad = 0;
        }

        if (!left) emit_repeat(out, ctx, ' ', pad);
        emit_chars(out, ctx, prefix, prefix_len);
        emit_repeat(out, ctx, '0', zeros);
        emit_chars(out, ctx, body, len);
        if (left) emit_repeat(out, ctx, ' ', pad);
        count += pad + prefix_len + zeros + len;
    }
    return count;
}

typedef struct {
    char* buf;
    size_t size;
    size_t pos;
} buffer_ctx_t;

static void buffer_out(char c, void* ctx) {
    buffer_ctx_t* b = ctx;
    if (b->pos + 1 < b->size) { // always leave room for the '\0'
        b->buf[b->pos] = c;
    }
    b->pos++;
}

int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
    buffer_ctx_t ctx = { buf, size, 0 };
    int n = vformat(buffer_out, &ctx, fmt, ap);
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

static void console_out(char c, void* ctx) {
    (void)ctx;
    console_putchar_raw(c);
}

int vprintf(const char* fmt, va_list ap) {
    int n = vformat(console_out, NULL, fmt, ap);
    console_update_cursor(); // once at the end, not after every character
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

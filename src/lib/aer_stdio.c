/*
    aer_stdio.c
    Formatted output (freestanding replacement for the output half of <stdio.h>).
    Every printf style function feeds the same formatting engine, only the place
    the characters end up (console or buffer) differs.

    How it fits together:

        prtf -----> vprtf ------\                          /--> console_out --> VGA console
                                 >--> vformat(out, ctx) --<                     (+ serial port)
        sprintf --> vsprintf ---\                          \--> buffer_out ---> your char buffer
        sfmt -> vsnprintf --/

    vformat is the engine. It walks the format string, and for each "%..." it reads the
    next argument, turns it into characters and passes them one at a time to an "output
    function" (out_fn). It doesn't know or care where they go. The ctx pointer is passed
    through untouched so each output function can keep its own state: buffer_out keeps
    the buffer and how full it is, console_out doesn't need any. Adding another
    destination (say, printing straight to the serial port) is just one more small
    output function.

    The format string grammar (flags, width, precision, length, conversion) is
    described in aer_stdio.h.

    Printing 64 bit numbers on a 32 bit CPU needs a trick, see divmod64 below.

    Handy references:
        https://en.cppreference.com/w/c/io/fprintf
        https://wiki.osdev.org/Printing_To_Screen
        https://wiki.osdev.org/Libgcc
*/
#include "aer_stdio.h"
#include <stdbool.h>
#include <stdint.h>
#include "aer_ctype.h"
#include "aer_string.h"
#include "../drivers/video/VGA_linear.h"

// an "output function": vformat calls it once for every character it produces
// c   = the character
// ctx = whatever the caller handed to vformat (e.g. a buffer_ctx_t), passed through unchanged
typedef void (*out_fn)(char c, void* ctx);

// divide a 64 bit number by a 32 bit one without needing libgcc (__udivdi3), returns the remainder
// *n is replaced with the quotient, so calling this in a loop peels off one digit at a time
//
// Why not just write *n / divisor? On 32 bit x86 the CPU can't divide a 64 bit number in one
// go, so GCC turns that into a call to __udivdi3 / __umoddi3 in libgcc, which we don't link.
//
// What the CPU CAN do: "divl" divides the 64 bit value in EDX:EAX (EDX = top half, EAX = bottom
// half) by a 32 bit number, putting the quotient in EAX and the remainder in EDX. The catch is that
// the quotient must fit in 32 bits, otherwise the CPU raises a divide error (exception 0) and the
// kernel would crash.
//
// So we do long division in base 2^32, exactly like long division on paper but where each
// "digit" is a whole 32 bit half:
//   1. divide the high half on its own (ordinary 32 bit division), keeping the remainder
//   2. put that remainder in front of the low half and divide again with divl
// Because the remainder from step 1 is smaller than the divisor, the quotient of step 2 always fits.
static uint32_t divmod64(unsigned long long* n, uint32_t divisor) {
    uint32_t high = (uint32_t)(*n >> 32);
    uint32_t low = (uint32_t)*n;
    // step 1: the top half of the quotient, and what's left over to carry down
    uint32_t q_high = high / divisor;
    uint32_t rem = high % divisor;
    uint32_t q_low;
    // rem < divisor, so (rem:low) / divisor always fits in 32 bits
    // step 2, reading the asm constraints:
    //   "=a"(q_low)    output: the quotient comes back in EAX
    //   "=d"(rem)      output: the remainder comes back in EDX
    //   "0"(low)       input: low goes in the same register as output 0 (EAX)
    //   "1"(rem)       input: rem goes in the same register as output 1 (EDX), the top half of EDX:EAX
    //   "rm"(divisor)  input: the divisor, in any register or in memory; it's operand number 4,
    //                  hence "divl %4"
    __asm__ ("divl %4" : "=a"(q_low), "=d"(rem) : "0"(low), "1"(rem), "rm"(divisor));
    // glue the two halves of the quotient back together
    *n = ((unsigned long long)q_high << 32) | q_low;
    return rem;
}

// write the digits of value so they end just before end, returns where they start
// we get digits from the least significant end (value % base), so it's easiest to fill the buffer
// backwards from the end: no reversing needed afterwards. The result is NOT terminated, the
// caller works out the length from (end - start). Bases up to 16 only, upper picks "ABCDEF" over "abcdef".
static char* format_digits(unsigned long long value, uint32_t base, bool upper, char* end) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    // do/while so that 0 still produces one "0" digit
    do {
        *--end = digits[divmod64(&value, base)];
    } while (value != 0);
    return end;
}

// send the character c to the output n times (used for padding with spaces or zeros)
// n <= 0 sends nothing
static void emit_repeat(out_fn out, void* ctx, char c, int n) {
    for (int i = 0; i < n; i++) {
        out(c, ctx);
    }
}

// send the first n characters of s to the output (s doesn't have to be terminated)
static void emit_chars(out_fn out, void* ctx, const char* s, int n) {
    for (int i = 0; i < n; i++) {
        out(s[i], ctx);
    }
}

// the formatting engine shared by every printf style function
// out = where each character goes, ctx = passed through to out, fmt = the format string,
// ap = the arguments. Returns the number of characters produced, which is what prtf returns.
// (for sfmt that includes characters that didn't fit in the buffer)
static int vformat(out_fn out, void* ctx, const char* fmt, va_list ap) {
    int count = 0;

    for (; *fmt; fmt++) {
        // ordinary characters are copied straight through
        if (*fmt != '%') {
            out(*fmt, ctx);
            count++;
            continue;
        }
        // step past the '%', fmt now points at the first character of the specifier
        fmt++;

        // flags
        // any number of them, in any order, e.g. "%-+5d" or "%+-5d"
        bool left = false, zero = false, plus = false, space = false, alt = false;
        for (;; fmt++) {
            if (*fmt == '-') {
                left = true;
            }
            else if (*fmt == '0') {
                zero = true;
            }
            else if (*fmt == '+') {
                plus = true;
            }
            else if (*fmt == ' ') {
                space = true;
            }
            else if (*fmt == '#') {
                alt = true;
            }
            else break;
        }

        // width, a negative * width means left aligned
        // (a '0' can't start the width, the flag loop above already took it as the zero flag,
        // which is exactly why "%08x" means "zero pad to width 8")
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = true;
                width = -width;
            }
            fmt++;
        } else {
            // read a decimal number one digit at a time: "12" -> 1, then 1 * 10 + 2
            while (isdigit((unsigned char)*fmt)) {
                width = width * 10 + (*fmt++ - '0');
            }
        }

        // precision, -1 when not given
        // a '.' with no number after it means a precision of 0
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                // the standard says a negative * precision acts as if no precision was given
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
        // on 32 bit x86 size_t (z) and ptrdiff_t (t) are the same size as long, while
        // intmax_t (j) is 64 bits like long long, so they map onto the existing sizes
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

        // the conversion character itself
        char spec = *fmt;
        // the format ended part way through a specifier (e.g. "100%"), stop rather than run off the end
        if (spec == '\0') {
            break;
        }

        // every conversion fills in these, then the shared padding code below prints them as:
        //     [spaces] [prefix] [zeros] [body] [spaces]
        // e.g. "%#8.4x" with 0xAB gives "  0x00ab": 2 spaces, prefix "0x", zeros "00", body "ab"
        char buf[64]; // enough for a 64 bit number in binary
        char* buf_end = buf + sizeof(buf);
        const char* body = NULL;   // the main text to print (digits, the string, the character)
        int len = 0;               // how many characters of body to print
        const char* prefix = "";   // printed before any zeros: a sign, or "0x" / "0" / "0b"
        bool is_number = false;    // numbers get precision and zero padding, strings don't

        switch (spec) {
            case 'd':
            case 'i': {
                // signed integers: always read into a long long so one code path handles every size
                long long value;
                if (length == 2) {
                    value = va_arg(ap, long long);
                }
                else if (length == 1) {
                    value = va_arg(ap, long);
                }
                else value = va_arg(ap, int);
                // anything smaller than an int is "promoted" to int when passed through "...",
                // so for h / hh we read an int and then cut it back down to the size it really was
                if (length == -1) {
                    value = (short)value;
                }
                if (length == -2) {
                    value = (signed char)value;
                }

                // work out the sign prefix; '+' beats ' ' if both flags are given
                if (value < 0) {
                    prefix = "-";
                }
                else if (plus) {
                    prefix = "+";
                }
                else if (space) {
                    prefix = " ";
                }
                // negate as unsigned so the most negative value works too
                // (-LLONG_MIN doesn't fit in a long long, but wraps round to the right magnitude as unsigned)
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
                // unsigned integers in some base, read the same way as above
                unsigned long long value;
                if (spec == 'p') {
                    // a pointer is printed as hex with a "0x" in front, so turn the # flag on
                    // (uintptr_t is the integer type that's exactly the size of a pointer)
                    value = (uintptr_t)va_arg(ap, void*);
                    alt = true;
                } else if (length == 2) value = va_arg(ap, unsigned long long);
                else if (length == 1) {
                    value = va_arg(ap, unsigned long);
                }
                else value = va_arg(ap, unsigned int);
                if (length == -1) {
                    value = (unsigned short)value;
                }
                if (length == -2) {
                    value = (unsigned char)value;
                }

                uint32_t base = 10;
                if (spec == 'x' || spec == 'X' || spec == 'p') {
                    base = 16;
                }
                else if (spec == 'o') {
                    base = 8;
                }
                else if (spec == 'b') {
                    base = 2;
                }

                // the # flag's prefix; like the C library, zero gets no prefix (so a NULL %p prints "0")
                if (alt && value != 0) {
                    if (base == 16) {
                        prefix = (spec == 'X') ? "0X" : "0x";
                    }
                    else if (base == 8) {
                        prefix = "0";
                    }
                    else if (base == 2) {
                        prefix = "0b";
                    }
                }
                body = format_digits(value, base, spec == 'X', buf_end);
                is_number = true;
                break;
            }
            case 'c':
                // a char is promoted to int when passed through "...", so read an int and keep the low byte
                buf[0] = (char)va_arg(ap, int);
                body = buf;
                len = 1;
                break;
            case 's':
                body = va_arg(ap, const char*);
                // printing "(null)" is far friendlier than reading whatever happens to be at address 0
                if (body == NULL) {
                    body = "(null)";
                }
                // with a precision, print at most that many characters and never read past them
                // (so "%.*s" can print part of a buffer that has no terminator)
                len = (precision >= 0) ? (int)strnlen(body, (size_t)precision) : (int)slen(body);
                break;
            case '%':
                body = "%";
                len = 1;
                break;
            default:
                // unknown conversion, print it as-is
                // (any flags or width that came before it are dropped)
                out('%', ctx);
                out(spec, ctx);
                count += 2;
                // continue jumps to the outer for loop's fmt++, moving past the unknown character
                continue;
        }

        // now the shared layout: work out how many zeros and spaces go round the body
        int zeros = 0;
        if (is_number) {
            // format_digits filled the END of buf, so the digits run from body up to buf_end
            len = (int)(buf_end - body);
            // precision = minimum number of digits (and ".0" with value 0 prints nothing)
            if (precision == 0 && len == 1 && body[0] == '0') {
                len = 0;
            }
            // e.g. "%.5d" with 42 needs 3 zeros to make 5 digits: "00042"
            if (precision > len) {
                zeros = precision - len;
            }
        }
        int prefix_len = (int)slen(prefix);
        // whatever the width asks for beyond what we already have becomes padding
        int pad = width - prefix_len - zeros - len;
        if (pad < 0) {
            pad = 0;
        }
        // the 0 flag: turn the padding into zeros instead of spaces. They go after the prefix,
        // so "%06d" with -42 is "-00042", not "000-42". The standard says the 0 flag is
        // ignored when left aligning (zeros on the right would change the number) or when
        // a precision was given (the precision already decides the zeros).
        if (is_number && zero && !left && precision < 0) {
            zeros += pad;
            pad = 0;
        }

        // finally print the pieces in order; the spaces go on the left normally, or on the right with '-'
        if (!left) {
            emit_repeat(out, ctx, ' ', pad);
        }
        emit_chars(out, ctx, prefix, prefix_len);
        emit_repeat(out, ctx, '0', zeros);
        emit_chars(out, ctx, body, len);
        if (left) {
            emit_repeat(out, ctx, ' ', pad);
        }
        count += pad + prefix_len + zeros + len;
    }
    return count;
}

// the state buffer_out needs to fill a caller's buffer safely
typedef struct {
    char* buf;     // the caller's buffer
    size_t size;   // its full size in bytes, including room for the '\0'
    size_t pos;    // how many characters have been produced so far (can go beyond size)
} buffer_ctx_t;

// output function for the sprintf family: stores c in the buffer if there's room
// once the buffer is full the characters are dropped, but pos still counts them,
// which is how vsnprintf can report the length the full result would have needed
static void buffer_out(char c, void* ctx) {
    // ctx is the buffer_ctx_t that vsnprintf passed to vformat
    buffer_ctx_t* b = ctx;
    if (b->pos + 1 < b->size) { // always leave room for the '\0'
        b->buf[b->pos] = c;
    }
    b->pos++;
}

// print into buf (size bytes long), returns the full length the result needed
// the result is cut short if it doesn't fit, but it's always terminated, and if size is 0 the buffer
// isn't touched at all (so buf may be NULL). A neat trick: vsnprintf(NULL, 0, ...) + 1 tells you
// exactly how big a buffer to malloc.
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
    buffer_ctx_t ctx = { buf, size, 0 };
    int n = vformat(buffer_out, &ctx, fmt, ap);
    if (size > 0) {
        // terminate straight after the text if it all fit, otherwise in the very last byte
        buf[(ctx.pos < size) ? ctx.pos : size - 1] = '\0';
    }
    return n;
}

// sfmt is just vsnprintf with the "..." arguments gathered into a va_list
// va_start points ap at the arguments after fmt, va_end tidies up afterwards
int sfmt(char* buf, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

// print into buf with no size limit (like the standard, it trusts that buf is big enough)
// we reuse vsnprintf and pretend the buffer is INT32_MAX (about 2GB) long, which in practice means
// "unlimited"; a too small buf will be overrun, so prefer sfmt where you can
int vsprintf(char* buf, const char* fmt, va_list ap) {
    return vsnprintf(buf, (size_t)INT32_MAX, fmt, ap);
}

// print into buf with no size limit, see vsprintf
int sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

// output function for prtf: puts the character on the console (which also mirrors it to the serial port)
// it uses the "raw" version that doesn't move the blinking hardware cursor, because moving it
// means talking to the VGA controller through I/O ports, which is slow to do for every character
static void console_out(char c, void* ctx) {
    // the console needs no state, so ctx is unused (the cast stops the "unused parameter" warning)
    (void)ctx;
    console_putchar_raw(c);
}

// prtf with a va_list: format straight onto the console
int vprtf(const char* fmt, va_list ap) {
    int n = vformat(console_out, NULL, fmt, ap);
    console_update_cursor(); // once at the end, not after every character
    return n;
}

// print to the console, e.g. prtf("%d files, %u KB free\n", count, kb)
// returns the number of characters printed
int prtf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprtf(fmt, ap);
    va_end(ap);
    return n;
}

// print a single character to the console and move the cursor after it
// returns the character as an unsigned value (0..255), like the standard, so a successful
// putchar(0xFF) can never be mistaken for EOF (-1)
int putchar(int c) {
    console_putchar((char)c);
    return (unsigned char)c;
}

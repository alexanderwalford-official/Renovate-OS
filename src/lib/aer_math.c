/*
    aer_math.c - maths on decimal numbers (see aer_math.h)

    The x87 FPU works on a stack of eight registers, st(0) at the top. In GCC's inline assembly
    "t" means st(0) and "u" means st(1), so

        __asm__ ("fsqrt" : "=t"(result) : "0"(x));

    puts x in st(0), runs fsqrt (which replaces st(0) with its square root), and takes st(0) back
    out as the result.
*/

#include "aer_math.h"
#include "aer_string.h"
#include "aer_ctype.h"
#include <stdint.h>

double sqrt (double x) {
    double result;
    __asm__ ("fsqrt" : "=t"(result) : "0"(x));
    return result;
}

double sin (double x) {
    double result;
    __asm__ ("fsin" : "=t"(result) : "0"(x));
    return result;
}

double cos (double x) {
    double result;
    __asm__ ("fcos" : "=t"(result) : "0"(x));
    return result;
}

double atan2 (double y, double x) {
    // fpatan works out atan(st(1) / st(0)), using the signs of both to get the right quarter,
    // and pops the stack: so x goes in st(0), y in st(1), and the answer is left in st(0)
    double result;
    __asm__ ("fpatan" : "=t"(result) : "0"(x), "u"(y) : "st(1)");
    return result;
}

bool is_nan (double x) {
    return x != x; // the one value that isn't equal to itself
}

bool is_infinite (double x) {
    return !is_nan(x) && is_nan(x - x); // inf - inf is nan; anything finite - itself is 0
}

double fabs (double x) {
    return x < 0 ? -x : x;
}

double floor (double x) {
    // beyond 2^53 every double is a whole number already (and nan / inf stay as they are)
    if (is_nan(x) || fabs(x) >= 9007199254740992.0) {
        return x;
    }
    double whole = (double)(int64_t)x;   // towards zero: 2.7 -> 2, -2.7 -> -2
    return whole > x ? whole - 1 : whole; // so for negatives, one less: -2.7 -> -3
}

double fmod (double x, double y) {
    if (y == 0 || is_nan(x) || is_nan(y) || is_infinite(x)) {
        return 0.0 / 0.0;
    }
    double quotient = x / y;
    double whole = quotient < 0 ? -floor(-quotient) : floor(quotient); // towards zero
    return x - whole * y;
}

// ---- turning numbers into text, and back -------------------------------------------------------

// Add a whole number (no sign, below 2^53) to out at *at. The digits are peeled off with double
// arithmetic, which is exact for whole numbers this size: 64 bit integer division would need a
// helper from libgcc on a 32 bit processor, which the kernel doesn't link
static void add_digits (double value, char* out, size_t size, size_t* at) {
    char digits[24];
    int count = 0;
    do {
        double tens = floor(value / 10);
        digits[count++] = (char)('0' + (int)(value - tens * 10));
        value = tens;
    } while (value > 0 && count < (int)sizeof(digits));
    while (count > 0 && *at + 1 < size) {
        out[(*at)++] = digits[--count];
    }
}

static void add_text (const char* text, char* out, size_t size, size_t* at) {
    while (*text != '\0' && *at + 1 < size) {
        out[(*at)++] = *text++;
    }
}

size_t format_real (double x, char* out, size_t size) {
    size_t at = 0;
    if (is_nan(x)) {
        add_text("nan", out, size, &at);
    } else if (is_infinite(x)) {
        add_text(x < 0 ? "-inf" : "inf", out, size, &at);
    } else {
        if (x < 0) {
            add_text("-", out, size, &at);
            x = -x;
        }
        // very big or very small: as d.dddddd e+n, so the six figures shown are the ones that count
        int exponent = 0;
        bool scientific = x >= 1e15 || (x != 0 && x < 1e-5);
        if (scientific) {
            while (x >= 10) {
                x /= 10;
                exponent++;
            }
            while (x < 1) {
                x *= 10;
                exponent--;
            }
            // rounding to six decimals can make 9.9999999 into 10.0 (0.000001 is really
            // 0.000000999999... in binary): then it is 1.0 with the exponent one higher
            if (floor(x * 1e6 + 0.5) >= 1e7) {
                x /= 10;
                exponent++;
            }
        }
        // the whole part, and the six decimals as a number of millionths (rounded: 0.9999999
        // becomes 1.0, carrying into the whole part)
        double whole = floor(x);
        uint32_t fraction = (uint32_t)((x - whole) * 1e6 + 0.5);
        if (fraction >= 1000000) {
            fraction -= 1000000;
            whole += 1;
        }
        add_digits(whole, out, size, &at);
        add_text(".", out, size, &at);
        // the decimals, without the 0s on the end (but always at least one: "2.0")
        char decimals[8];
        for (int i = 5; i >= 0; i--) {
            decimals[i] = (char)('0' + fraction % 10);
            fraction /= 10;
        }
        int keep = 6;
        while (keep > 1 && decimals[keep - 1] == '0') {
            keep--;
        }
        decimals[keep] = '\0';
        add_text(decimals, out, size, &at);
        if (scientific) {
            add_text(exponent < 0 ? "e-" : "e+", out, size, &at);
            add_digits((double)(exponent < 0 ? -exponent : exponent), out, size, &at);
        }
    }
    out[at] = '\0';
    return at;
}

bool parse_real (const char* text, double* out, const char** end) {
    const char* p = text;
    bool negative = *p == '-';
    if (*p == '-' || *p == '+') {
        p++;
    }
    if (!isdigit((unsigned char)*p) && !(*p == '.' && isdigit((unsigned char)p[1]))) {
        *end = text;
        return false;
    }
    double value = 0;
    while (isdigit((unsigned char)*p)) {
        value = value * 10 + (*p++ - '0');
    }
    if (*p == '.') {
        p++;
        double place = 0.1;
        while (isdigit((unsigned char)*p)) {
            value += (*p++ - '0') * place;
            place /= 10;
        }
    }
    // an exponent: e or E, a sign, digits (if what follows isn't digits, it isn't one)
    if ((*p == 'e' || *p == 'E') && (isdigit((unsigned char)p[1]) ||
        ((p[1] == '-' || p[1] == '+') && isdigit((unsigned char)p[2])))) {
        p++;
        bool down = *p == '-';
        if (*p == '-' || *p == '+') {
            p++;
        }
        int exponent = 0;
        while (isdigit((unsigned char)*p)) {
            if (exponent < 1000) {
                exponent = exponent * 10 + (*p - '0');
            }
            p++;
        }
        for (int i = 0; i < exponent; i++) {
            value = down ? value / 10 : value * 10;
        }
    }
    *out = negative ? -value : value;
    *end = p;
    return true;
}

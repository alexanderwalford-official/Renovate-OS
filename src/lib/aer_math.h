/*
    aer_math.h - maths on decimal numbers (doubles), for AerLang's floating point

    The kernel has no C library, so these are AerOS's own. Most are a single instruction of the
    x87 FPU, the part of the processor that works with decimal numbers (see lib/fpu.h: it must
    be switched on with fpu_enable() before any of these are used):

        sqrt    fsqrt     square root
        sin     fsin      sine, of an angle in radians (exact for anything a program would use)
        cos     fcos      cosine
        atan2   fpatan    the angle of the point (x, y), from -PI to PI
        floor             the whole number at or below x
        fabs              x without its sign
        fmod              the remainder of x / y, with the sign of x (like C's)

    A double holds about 15 significant digits, and numbers up to about 1.8e308.
*/
#ifndef AER_MATH_H
#define AER_MATH_H

#include <stddef.h>
#include <stdbool.h>

#define AER_PI 3.14159265358979323846

double sqrt (double x);
double sin (double x);
double cos (double x);
double atan2 (double y, double x);
double floor (double x);
double fabs (double x);
double fmod (double x, double y);

// is x "not a number" (nan, from 0.0 / 0.0 or sqrt(-1)), or infinite?
bool is_nan (double x);
bool is_infinite (double x);

// a double as text, the way AerLang prints it: at least one decimal, and up to 6 ("2.0",
// "3.141593", "0.5"), or with an exponent when very big or very small ("1.5e+20"). "nan",
// "inf" and "-inf" for those. Returns the length
size_t format_real (double x, char* out, size_t size);

// read a decimal number from text: digits, an optional "." and more digits, and an optional
// exponent ("2.5", "-3", "1e6", "4.2E-3"). *end is set to the first character after it. False
// (with *end = text) if there is no number there
bool parse_real (const char* text, double* out, const char** end);

#endif /* AER_MATH_H */

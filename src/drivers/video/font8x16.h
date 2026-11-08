/*
    font8x16.h
    The built-in 8x16 font used when the screen is a framebuffer instead of VGA text mode.
*/
#ifndef FONT8X16_H
#define FONT8X16_H

#include <stdint.h>

// one glyph per code page 437 character, 16 rows from the top, bit 7 = leftmost pixel
extern const uint8_t font8x16[256][16];

#endif /* FONT8X16_H */

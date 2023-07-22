// rendering the screen in a linear VGA framebuffer
// https://wiki.osdev.org/Drawing_In_a_Linear_Framebuffer

#ifndef VGA_LINEAR_H
#define VGA_LINEAR_H

#include "stdint.h"

int puthandler () {
    return 0;
}

enum color {
    BLACK = 0,
    DARK_BLUE = 1,
    GREEN = 2,
    CYAN = 3,
    RED = 4,
    PURPLE = 5,
    BROWN = 6,
    BRIGHT = 7,
    DARK_GRAY = 8,
    LIGHT_BLUE = 9,
    LIGHT_GREEN = 10,
    LIGHT_CYAN = 11,
    LIGHT_RED = 12,
    LIGHT_PURPLE = 13,
    YELLOW = 14,
    WHITE = 15
};

enum size {
    COLS = 80,
    ROWS = 25
};

// unsigned 16 bit constant for video memory register
uint16_t *const video = (uint16_t*) 0xB8000;

void putc(uint8_t x, uint8_t y, enum color fg, enum color bg, char c) {
    video[y * COLS + x] = (bg << 12) | (fg << 8) | c;
}

// COLLUMN, ROW, FOREGROUND COLOR, BACKGROUND COLOR, TEXT
void puts(uint8_t x, uint8_t y, enum color fg, enum color bg, const char *s) {
    for (; *s; s++, x++)
        putc(x, y, fg, bg, *s);
}

// clear the screen, line by line
void clear(enum color bg) {
    uint8_t x, y;
    for (y = 0; y < ROWS; y++) {
        for (x = 0; x < COLS; x++) {
            putc(x, y, bg, bg, ' ');
        }     
    }
}
#endif /* VGA_LINEAR */
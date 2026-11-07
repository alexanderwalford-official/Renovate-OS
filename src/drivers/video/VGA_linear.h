// rendering the screen in the VGA text mode buffer (80x25 characters at 0xB8000)
// https://wiki.osdev.org/Printing_To_Screen
// https://wiki.osdev.org/Text_Mode_Cursor

#ifndef VGA_LINEAR_H
#define VGA_LINEAR_H

#include <stdint.h>
#include <stdbool.h>
#include "../input/GIO.h"
#include "../serial/serial.h"

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

// VGA CRT controller ports (cursor control)
#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5

// build a 16 bit VGA cell: low byte = character, high byte = colour attribute
// (the character is cast to uint8_t so characters above 127 do not sign-extend into the colour)
#define VGA_ENTRY(c, fg, bg) ((uint16_t)((uint8_t)(c) | ((uint16_t)((((bg) & 0x0F) << 4) | ((fg) & 0x0F)) << 8)))

// video memory, volatile because it is read by the graphics hardware and not just this program
volatile uint16_t *const video = (volatile uint16_t*) 0xB8000;

// counts full screen clears, the terminal uses it to notice when a program took over the screen
volatile unsigned int vga_clear_count = 0;

/* ------------------------------------------------------------------ */
/*  positional drawing (fixed column / row)                             */
/* ------------------------------------------------------------------ */

// draw a single character at a COLUMN, ROW (anything off screen is ignored)
void putc(uint8_t x, uint8_t y, enum color fg, enum color bg, char c) {
    if (x >= COLS || y >= ROWS) {
        return;
    }
    video[y * COLS + x] = VGA_ENTRY(c, fg, bg);
}

// COLUMN, ROW, FOREGROUND COLOR, BACKGROUND COLOR, TEXT
// long text wraps onto the next row, '\n' moves down a row back to the starting column
void puts(uint8_t x, uint8_t y, enum color fg, enum color bg, const char *s) {
    if (s == 0) {
        return;
    }
    unsigned int cx = x;
    unsigned int cy = y;
    for (; *s; s++) {
        if (*s == '\n') {
            cx = x;
            cy++;
            continue;
        }
        if (cx >= COLS) {
            cx = 0;
            cy++;
        }
        if (cy >= ROWS) {
            return;
        }
        putc((uint8_t)cx, (uint8_t)cy, fg, bg, *s);
        cx++;
    }
}

/* ------------------------------------------------------------------ */
/*  hardware cursor                                                     */
/* ------------------------------------------------------------------ */

bool vga_cursor_visible = false;

// show the blinking hardware cursor, using scanlines start..end of the character cell
void vga_enable_cursor(uint8_t start, uint8_t end) {
    outb(VGA_CRTC_INDEX, 0x0A);
    outb(VGA_CRTC_DATA, (inb(VGA_CRTC_DATA) & 0xC0) | start);
    outb(VGA_CRTC_INDEX, 0x0B);
    outb(VGA_CRTC_DATA, (inb(VGA_CRTC_DATA) & 0xE0) | end);
    vga_cursor_visible = true;
}

// hide the hardware cursor
void vga_disable_cursor(void) {
    outb(VGA_CRTC_INDEX, 0x0A);
    outb(VGA_CRTC_DATA, 0x20);
    vga_cursor_visible = false;
}

// move the hardware cursor
void vga_move_cursor(uint8_t x, uint8_t y) {
    uint16_t pos = (uint16_t)(y * COLS + x);
    outb(VGA_CRTC_INDEX, 0x0F);
    outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));
    outb(VGA_CRTC_INDEX, 0x0E);
    outb(VGA_CRTC_DATA, (uint8_t)((pos >> 8) & 0xFF));
}

/* ------------------------------------------------------------------ */
/*  console (a scrolling terminal that keeps track of the cursor)       */
/* ------------------------------------------------------------------ */

uint8_t console_x = 0;
uint8_t console_y = 0;
enum color console_fg = BRIGHT;
enum color console_bg = BLACK;

// clear the screen, line by line
void clear(enum color bg) {
    uint8_t x, y;
    for (y = 0; y < ROWS; y++) {
        for (x = 0; x < COLS; x++) {
            putc(x, y, bg, bg, ' ');
        }
    }
    // full screen layouts (splash, error screens) draw with puts, so hide the console cursor
    console_x = 0;
    console_y = 0;
    vga_disable_cursor();
    vga_clear_count++;
}

void console_set_color(enum color fg, enum color bg) {
    console_fg = fg;
    console_bg = bg;
}

void console_update_cursor(void) {
    if (!vga_cursor_visible) {
        vga_enable_cursor(14, 15);
    }
    vga_move_cursor(console_x, console_y);
}

void console_set_cursor(uint8_t x, uint8_t y) {
    console_x = (x < COLS) ? x : COLS - 1;
    console_y = (y < ROWS) ? y : ROWS - 1;
    console_update_cursor();
}

void console_get_cursor(uint8_t* x, uint8_t* y) {
    if (x) *x = console_x;
    if (y) *y = console_y;
}

// clear the screen using the console colours and move the cursor to the top left
void console_clear(void) {
    for (unsigned int i = 0; i < COLS * ROWS; i++) {
        video[i] = VGA_ENTRY(' ', console_fg, console_bg);
    }
    console_x = 0;
    console_y = 0;
    console_update_cursor();
}

// move every line up by one and blank the bottom line
void console_scroll(void) {
    for (unsigned int i = 0; i < COLS * (ROWS - 1); i++) {
        video[i] = video[i + COLS];
    }
    for (unsigned int x = 0; x < COLS; x++) {
        video[(ROWS - 1) * COLS + x] = VGA_ENTRY(' ', console_fg, console_bg);
    }
}

// write one character at the cursor, handling newlines, tabs, backspace, wrapping and scrolling
void console_putchar_raw(char c) {
    switch (c) {
        case '\n':
            console_x = 0;
            console_y++;
            break;
        case '\r':
            console_x = 0;
            break;
        case '\b':
            if (console_x > 0) {
                console_x--;
            } else if (console_y > 0) {
                console_y--;
                console_x = COLS - 1;
            }
            break;
        case '\t': {
            uint8_t next = (uint8_t)((console_x + 4) & ~3);
            while (console_x < next && console_x < COLS) {
                putc(console_x++, console_y, console_fg, console_bg, ' ');
            }
            break;
        }
        default:
            putc(console_x, console_y, console_fg, console_bg, c);
            console_x++;
            break;
    }

    if (console_x >= COLS) {
        console_x = 0;
        console_y++;
    }
    if (console_y >= ROWS) {
        console_scroll();
        console_y = ROWS - 1;
    }

    // mirror to the serial port for debugging
    if (c == '\n') {
        serial_putchar('\r');
    }
    if (c != '\b') {
        serial_putchar(c);
    }
}

void console_putchar(char c) {
    console_putchar_raw(c);
    console_update_cursor();
}

// write a string at the cursor
void console_write(const char* s) {
    if (s == 0) {
        return;
    }
    while (*s) {
        console_putchar_raw(*s++);
    }
    console_update_cursor();
}

// erase the character before the cursor (used by the line editor)
void console_backspace(void) {
    console_putchar_raw('\b');
    putc(console_x, console_y, console_fg, console_bg, ' ');
    serial_write("\b \b");
    console_update_cursor();
}

// one-time VGA setup: allow all 16 background colours (instead of blinking text) and show the cursor
void vga_init(void) {
    inb(0x3DA);              // reset the attribute controller flip-flop
    outb(0x3C0, 0x30);       // select the mode control register (bit 5 keeps the screen on)
    uint8_t mode = inb(0x3C1);
    outb(0x3C0, mode & ~0x08); // clear the blink bit
    console_set_color(BRIGHT, BLACK);
    console_clear();
}

#endif /* VGA_LINEAR */

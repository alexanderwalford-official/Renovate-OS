// rendering the screen in the VGA text mode buffer (80x25 characters at 0xB8000)
// https://wiki.osdev.org/Printing_To_Screen
// https://wiki.osdev.org/Text_Mode_Cursor

#include "VGA_linear.h"

#include <stdint.h>
#include <stdbool.h>
#include "../input/GIO.h"
#include "../serial/serial.h"

// VGA CRT controller ports (cursor control)
#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5

volatile uint16_t* const video = (volatile uint16_t*)0xB8000;
volatile unsigned int vga_clear_count = 0;

enum color console_fg = BRIGHT;
enum color console_bg = BLACK;
static uint8_t console_x = 0;
static uint8_t console_y = 0;

static bool vga_cursor_visible = false;

void vga_enable_cursor(uint8_t start, uint8_t end) {
    outb(VGA_CRTC_INDEX, 0x0A);
    outb(VGA_CRTC_DATA, (inb(VGA_CRTC_DATA) & 0xC0) | start);
    outb(VGA_CRTC_INDEX, 0x0B);
    outb(VGA_CRTC_DATA, (inb(VGA_CRTC_DATA) & 0xE0) | end);
    vga_cursor_visible = true;
}

void vga_disable_cursor(void) {
    outb(VGA_CRTC_INDEX, 0x0A);
    outb(VGA_CRTC_DATA, 0x20);
    vga_cursor_visible = false;
}

void vga_move_cursor(uint8_t x, uint8_t y) {
    uint16_t pos = (uint16_t)(y * COLS + x);
    outb(VGA_CRTC_INDEX, 0x0F);
    outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));
    outb(VGA_CRTC_INDEX, 0x0E);
    outb(VGA_CRTC_DATA, (uint8_t)(pos >> 8));
}

static void vga_fill(uint16_t entry) {
    for (unsigned int i = 0; i < COLS * ROWS; i++) {
        video[i] = entry;
    }
}

void putc(uint8_t x, uint8_t y, enum color fg, enum color bg, char c) {
    if (x >= COLS || y >= ROWS) {
        return;
    }
    video[y * COLS + x] = VGA_ENTRY(c, fg, bg);
}

void puts(uint8_t x, uint8_t y, enum color fg, enum color bg, const char* s) {
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
        video[cy * COLS + cx] = VGA_ENTRY(*s, fg, bg);
        cx++;
    }
}

void clear(enum color bg) {
    vga_fill(VGA_ENTRY(' ', bg, bg));
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
    *x = console_x;
    *y = console_y;
}

void console_clear(void) {
    vga_fill(VGA_ENTRY(' ', console_fg, console_bg));
    console_x = 0;
    console_y = 0;
    console_update_cursor();
}

// move every line up by one and blank the bottom line
static void console_scroll(void) {
    for (unsigned int i = 0; i < COLS * (ROWS - 1); i++) {
        video[i] = video[i + COLS];
    }
    for (unsigned int x = 0; x < COLS; x++) {
        video[(ROWS - 1) * COLS + x] = VGA_ENTRY(' ', console_fg, console_bg);
    }
}

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
            putc(console_x++, console_y, console_fg, console_bg, c);
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

    // mirror to the serial port for debugging (backspace is mirrored by console_backspace)
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

void console_write(const char* s) {
    while (*s) {
        console_putchar_raw(*s++);
    }
    console_update_cursor();
}

void console_backspace(void) {
    console_putchar_raw('\b');
    putc(console_x, console_y, console_fg, console_bg, ' ');
    serial_write("\b \b");
    console_update_cursor();
}

void vga_init(void) {
    inb(0x3DA);                // reset the attribute controller flip-flop
    outb(0x3C0, 0x30);         // select the mode control register (bit 5 keeps the screen on)
    uint8_t mode = inb(0x3C1);
    outb(0x3C0, mode & ~0x08); // clear the blink bit
    console_set_color(BRIGHT, BLACK);
    console_clear();
}

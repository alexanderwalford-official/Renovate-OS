// rendering the screen in the VGA text mode buffer (80x25 characters at 0xB8000)
// https://wiki.osdev.org/Printing_To_Screen
// https://wiki.osdev.org/Text_Mode_Cursor

#ifndef VGA_LINEAR_H
#define VGA_LINEAR_H

#include <stdint.h>

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

// build a 16 bit VGA cell: low byte = character, high byte = colour attribute
// (the character is cast to uint8_t so characters above 127 do not sign-extend into the colour)
#define VGA_ENTRY(c, fg, bg) ((uint16_t)((uint8_t)(c) | ((uint16_t)((((bg) & 0x0F) << 4) | ((fg) & 0x0F)) << 8)))

// video memory, volatile because it is read by the graphics hardware and not just this program
extern volatile uint16_t* const video;

// counts full screen clears, the terminal uses it to notice when a program took over the screen
extern volatile unsigned int vga_clear_count;

extern enum color console_fg;
extern enum color console_bg;

// one-time VGA setup: allow all 16 background colours (instead of blinking text) and show the cursor
void vga_init(void);

// show the blinking hardware cursor, using scanlines start..end of the character cell
void vga_enable_cursor(uint8_t start, uint8_t end);
void vga_disable_cursor(void);
void vga_move_cursor(uint8_t x, uint8_t y);

// positional drawing, for full screen layouts (splash, error screens)

// draw a single character at a COLUMN, ROW (anything off screen is ignored)
void putc(uint8_t x, uint8_t y, enum color fg, enum color bg, char c);

// COLUMN, ROW, FOREGROUND COLOR, BACKGROUND COLOR, TEXT
// long text wraps onto the next row, '\n' moves down a row back to the starting column
void puts(uint8_t x, uint8_t y, enum color fg, enum color bg, const char* s);

// fill the screen with one colour and hide the console cursor
void clear(enum color bg);

// console: a scrolling terminal that keeps track of the cursor (and mirrors to the serial port)

void console_set_color(enum color fg, enum color bg);
void console_set_cursor(uint8_t x, uint8_t y);
void console_get_cursor(uint8_t* x, uint8_t* y);
void console_update_cursor(void);

// clear the screen using the console colours and move the cursor to the top left
void console_clear(void);

// write one character at the cursor without moving the hardware cursor (call console_update_cursor after)
void console_putchar_raw(char c);
void console_putchar(char c);
void console_write(const char* s);

// erase the character before the cursor (used by the line editor)
void console_backspace(void);

#endif /* VGA_LINEAR_H */

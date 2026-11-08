// rendering the screen in the VGA text mode buffer (80x25 characters at 0xB8000)
// https://wiki.osdev.org/Printing_To_Screen
// https://wiki.osdev.org/Text_Mode_Cursor
//
// GRUB leaves the graphics card in the standard 80x25 colour text mode. In this mode the card
// does all the drawing itself: it reads a grid of 80 x 25 "cells" out of video memory at physical
// address 0xB8000 and renders each one with its built-in font, many times a second. To put a
// letter on screen we just write a cell into that memory (memory mapped I/O, see GIO.h).
//
// Each cell is 16 bits:
//     bits 0-7   the character, in code page 437 (ASCII plus box drawing / accented characters)
//     bits 8-11  foreground colour (one of enum color below)
//     bits 12-14 background colour
//     bit 15     normally "blink", but vga_init switches blinking off so this becomes the
//                bright bit of the background, giving us all 16 background colours
// x86 is little endian, so in memory the character byte comes first and the colour byte second.
// The cell for column x, row y is video[y * COLS + x].
//
// The file offers two layers:
// - positional drawing (putc / puts / clear) for full screen layouts that place text exactly
// - a console that remembers a cursor, wraps, scrolls and mirrors everything to the serial port,
//   which is what printf and the terminal use

#ifndef VGA_LINEAR_H
#define VGA_LINEAR_H

#include <stdint.h>
#include <stdbool.h>

// the 16 colours of the standard VGA text palette (inherited from CGA)
// bit 3 is the "intensity" bit: colours 8-15 are the bright versions of colours 0-7
enum color {
    BLACK = 0,
    DARK_BLUE = 1,
    GREEN = 2,
    CYAN = 3,
    RED = 4,
    PURPLE = 5,    // magenta in most VGA documentation
    BROWN = 6,     // really dark yellow, CGA monitors bent it to brown on purpose
    BRIGHT = 7,    // light grey, the classic default text colour
    DARK_GRAY = 8,
    LIGHT_BLUE = 9,
    LIGHT_GREEN = 10,
    LIGHT_CYAN = 11,
    LIGHT_RED = 12,
    LIGHT_PURPLE = 13,
    YELLOW = 14,
    WHITE = 15
};

// dimensions of the text screen in character cells
enum size {
    COLS = 80,
    ROWS = 25
};

// build a 16 bit VGA cell: low byte = character, high byte = colour attribute
// (the character is cast to uint8_t so characters above 127 do not sign-extend into the colour)
// the attribute byte is (background << 4) | foreground, each masked to 4 bits so a bad colour value
// can't spill into the neighbouring field
#define VGA_ENTRY(c, fg, bg) ((uint16_t)((uint8_t)(c) | ((uint16_t)((((bg) & 0x0F) << 4) | ((fg) & 0x0F)) << 8)))

// video memory, volatile because it is read by the graphics hardware and not just this program
// (it points at 0xB8000, an array of COLS * ROWS cells built with VGA_ENTRY)
extern volatile uint16_t* const video;

// counts full screen clears, the terminal uses it to notice when a program took over the screen
extern volatile unsigned int vga_clear_count;

// the colours the console draws new text with (set them with console_set_color)
extern enum color console_fg;
extern enum color console_bg;

// a linear framebuffer the bootloader set up for us (UEFI PCs have no VGA text mode)
typedef struct {
    uint32_t address;            // physical address of the first pixel (must be below 4GB)
    uint32_t pitch;              // bytes from one line of pixels to the next
    uint32_t width, height;      // in pixels
    uint8_t bpp;                 // bits per pixel: 16, 24 or 32
    uint8_t red_pos, red_size;   // where each colour sits inside a pixel, and how many bits it has
    uint8_t green_pos, green_size;
    uint8_t blue_pos, blue_size;
} vga_framebuffer_t;

// one-time screen setup, the very first thing the kernel initialises
// framebuffer = NULL uses VGA text mode (switching blinking off so all 16 background colours work),
// otherwise the 80 x 25 screen is drawn into the framebuffer (falling back to text mode if it
// can't be used). Also clears the screen to light grey on black.
void vga_init(const vga_framebuffer_t* framebuffer);

// true if the screen is a framebuffer drawn by us, false for VGA text mode
bool vga_framebuffer_active(void);

// copy out the framebuffer's details for drawing graphics, false in VGA text mode
bool vga_framebuffer_info(vga_framebuffer_t* out);

// the pixel rectangle covered by cols x rows cells starting at col, row (framebuffer only),
// for drawing something finer than text in a screen layout (e.g. the splash's wordmark)
bool vga_cell_area(uint8_t col, uint8_t row, uint8_t cols, uint8_t rows,
                   uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h);

// carry on drawing on a different framebuffer (the card changed resolution), redrawing everything
// false if it can't be used (smaller than 640 x 400, or the screen isn't a framebuffer)
bool vga_change_framebuffer(const vga_framebuffer_t* info);

// forget what the console last drew, so its next output repaints the whole screen
// (needed after a program has drawn its own graphics over it)
void vga_invalidate(void);

// show the blinking hardware cursor, using scanlines start..end of the character cell
// (a cell is 16 scanlines tall: 14..15 is the usual underline, 0..15 a full block)
void vga_enable_cursor(uint8_t start, uint8_t end);

// hide the hardware cursor (console_update_cursor shows it again)
void vga_disable_cursor(void);

// move the hardware cursor to column x, row y (it does not check that the position is on screen)
void vga_move_cursor(uint8_t x, uint8_t y);

// palette: each of the 16 colours above is really an index into the VGA's 256 entry colour table
// (the DAC), so any of them can be turned into a different colour. r / g / b are 0 - 255, the
// hardware keeps the top 6 bits of each. Changes show up instantly on everything already on screen.
void vga_set_palette(enum color color, uint8_t r, uint8_t g, uint8_t b);
void vga_get_palette(enum color color, uint8_t* r, uint8_t* g, uint8_t* b);

// replace the picture of one character in the VGA font with our own 8x16 bitmap
// (glyph = 16 rows, top first, bit 7 = leftmost pixel). Every cell showing that character changes.
void vga_set_glyph(uint8_t character, const uint8_t* glyph);

// the font has no copyright sign, so vga_init draws one into this unused character (0x01, a smiley)
#define VGA_COPYRIGHT "\x01"

// positional drawing, for full screen layouts (splash, error screens)

// draw a single character at a COLUMN, ROW (anything off screen is ignored)
void putc(uint8_t x, uint8_t y, enum color fg, enum color bg, char c);

// COLUMN, ROW, FOREGROUND COLOR, BACKGROUND COLOR, TEXT
// long text wraps onto the next row, '\n' moves down a row back to the starting column
// (wrapped text restarts at column 0, and anything that would go below the last row is dropped)
void puts(uint8_t x, uint8_t y, enum color fg, enum color bg, const char* s);

// fill the screen with one colour and hide the console cursor
// also moves the console cursor home and bumps vga_clear_count
void clear(enum color bg);

// console: a scrolling terminal that keeps track of the cursor (and mirrors to the serial port)

// change the colours used for everything the console writes from now on (existing text keeps its colour)
void console_set_color(enum color fg, enum color bg);

// move the console cursor, positions past the edge are clamped to the last column / row
void console_set_cursor(uint8_t x, uint8_t y);

// read the console cursor position into *x / *y
void console_get_cursor(uint8_t* x, uint8_t* y);

// move the blinking hardware cursor to where the console cursor is (showing it again if it was hidden)
void console_update_cursor(void);

// clear the screen using the console colours and move the cursor to the top left
void console_clear(void);

// write one character at the cursor without moving the hardware cursor (call console_update_cursor after)
// handles '\n', '\r', '\b' (moves back without erasing) and '\t' (to the next multiple of 4 columns)
void console_putchar_raw(char c);

// write one character at the cursor and move the hardware cursor after it
void console_putchar(char c);

// write a NUL terminated string, the hardware cursor is only moved once at the end
void console_write(const char* s);

// erase the character before the cursor (used by the line editor)
void console_backspace(void);

#endif /* VGA_LINEAR_H */

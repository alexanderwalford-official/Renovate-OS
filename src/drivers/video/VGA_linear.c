// rendering the screen in the VGA text mode buffer (80x25 characters at 0xB8000)
// https://wiki.osdev.org/Printing_To_Screen
// https://wiki.osdev.org/Text_Mode_Cursor
//
// Drawing text is plain memory writes into the cell array at 0xB8000 (see VGA_linear.h for the
// cell format). The blinking cursor and the blink / bright background setting are different:
// they live in registers inside the VGA chip, reached through I/O ports.
//
// Many VGA registers hide behind an "index / data" pair of ports: you write the number of the
// register you want into the index port, then read or write its value through the data port.
// The CRT controller (CRTC), which produces the picture and the cursor, uses 0x3D4 / 0x3D5.
// The CRTC registers used here:
//     0x0A cursor start: bits 0-4 = first scanline of the cursor, bit 5 = cursor disable
//     0x0B cursor end:   bits 0-4 = last scanline of the cursor (bits 5-6 = cursor skew)
//     0x0E cursor location, high byte  } the cursor position as a cell index (y * COLS + x),
//     0x0F cursor location, low byte   } exactly the index used for video[]
// https://wiki.osdev.org/VGA_Hardware
//
// Nothing here is protected against interrupts, and it doesn't need to be: the timer and keyboard
// interrupt handlers never draw (the keyboard only queues keys, the line editor echoes them later),
// and the CPU exception handler that draws the red error screen never returns to what it interrupted.

#include "VGA_linear.h"

#include <stdint.h>
#include <stdbool.h>
#include "../input/GIO.h"
#include "../serial/serial.h"

// VGA CRT controller ports (cursor control)
// (these are the colour mode addresses, a monochrome card uses 0x3B4 / 0x3B5)
#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5

// the text buffer itself, AerOS has no paging so the physical address works as a pointer
volatile uint16_t* const video = (volatile uint16_t*)0xB8000;
volatile unsigned int vga_clear_count = 0;

enum color console_fg = BRIGHT;
enum color console_bg = BLACK;
// the console cursor: the cell the next character goes into (column 0-79, row 0-24)
// this is our own bookkeeping, the hardware cursor is only moved to match it by console_update_cursor
static uint8_t console_x = 0;
static uint8_t console_y = 0;

// whether the hardware cursor is currently switched on, so console_update_cursor only has to
// re-enable it after clear() hid it instead of reprogramming it on every character
static bool vga_cursor_visible = false;

// switch the hardware cursor on and set its shape
// start / end: first and last scanline (0-15 in the 16 pixel tall cell) the cursor covers
// the registers are read first so the bits we don't own are kept, and clearing bit 5 of register
// 0x0A (via the 0xC0 mask) is what actually turns the cursor on
void vga_enable_cursor(uint8_t start, uint8_t end) {
    outb(VGA_CRTC_INDEX, 0x0A);
    outb(VGA_CRTC_DATA, (inb(VGA_CRTC_DATA) & 0xC0) | start);
    outb(VGA_CRTC_INDEX, 0x0B);
    outb(VGA_CRTC_DATA, (inb(VGA_CRTC_DATA) & 0xE0) | end);
    vga_cursor_visible = true;
}

// switch the hardware cursor off: 0x20 sets the "cursor disable" bit (bit 5) of register 0x0A
void vga_disable_cursor(void) {
    outb(VGA_CRTC_INDEX, 0x0A);
    outb(VGA_CRTC_DATA, 0x20);
    vga_cursor_visible = false;
}

// move the hardware cursor to column x, row y
// the CRTC wants the linear cell index, split over two 8 bit registers (0x0F low, 0x0E high)
void vga_move_cursor(uint8_t x, uint8_t y) {
    uint16_t pos = (uint16_t)(y * COLS + x);
    outb(VGA_CRTC_INDEX, 0x0F);
    outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));
    outb(VGA_CRTC_INDEX, 0x0E);
    outb(VGA_CRTC_DATA, (uint8_t)(pos >> 8));
}

// write the same cell (character + colours) into all 80 * 25 = 2000 cells of the screen
// (a plain loop rather than memset: video memory is volatile and the kernel is built with -fno-builtin)
static void vga_fill(uint16_t entry) {
    for (unsigned int i = 0; i < COLS * ROWS; i++) {
        video[i] = entry;
    }
}

// draw character c with colours fg / bg at column x, row y, ignoring positions off the screen
// it doesn't touch the console cursor, so full screen layouts can draw anywhere
// gotcha: the name clashes with the C library's putc, which is fine in a freestanding kernel with
// -fno-builtin (the compiler won't assume it is the libc function)
void putc(uint8_t x, uint8_t y, enum color fg, enum color bg, char c) {
    if (x >= COLS || y >= ROWS) {
        return;
    }
    video[y * COLS + x] = VGA_ENTRY(c, fg, bg);
}

// draw a string starting at column x, row y
// cx / cy are the current drawing position, kept as unsigned int so they can run past 255 / the
// screen edge without wrapping round like a uint8_t would
void puts(uint8_t x, uint8_t y, enum color fg, enum color bg, const char* s) {
    unsigned int cx = x;
    unsigned int cy = y;
    for (; *s; s++) {
        // '\n' = start a new row lined up under the first character, handy for drawing boxes and menus
        if (*s == '\n') {
            cx = x;
            cy++;
            continue;
        }
        // ran off the right edge: carry on at the start of the next row
        if (cx >= COLS) {
            cx = 0;
            cy++;
        }
        // ran off the bottom: there is nowhere left to draw (puts never scrolls)
        if (cy >= ROWS) {
            return;
        }
        video[cy * COLS + cx] = VGA_ENTRY(*s, fg, bg);
        cx++;
    }
}

// fill the whole screen with colour bg (spaces drawn in bg on bg) and hand the screen over to
// positional drawing
void clear(enum color bg) {
    vga_fill(VGA_ENTRY(' ', bg, bg));
    // full screen layouts (splash, error screens) draw with puts, so hide the console cursor
    console_x = 0;
    console_y = 0;
    vga_disable_cursor();
    // the terminal compares this counter before and after running a command, a change means the
    // command drew its own screen and the terminal needs to redraw itself
    vga_clear_count++;
}

// set the colours the console uses for new characters (and for blank cells when it clears or scrolls)
void console_set_color(enum color fg, enum color bg) {
    console_fg = fg;
    console_bg = bg;
}

// put the blinking hardware cursor where the console cursor is
// if clear() hid it, it is first switched back on as an underline (scanlines 14-15)
void console_update_cursor(void) {
    if (!vga_cursor_visible) {
        vga_enable_cursor(14, 15);
    }
    vga_move_cursor(console_x, console_y);
}

// move the console cursor to column x, row y, clamping to the screen so later writes stay in bounds
void console_set_cursor(uint8_t x, uint8_t y) {
    console_x = (x < COLS) ? x : COLS - 1;
    console_y = (y < ROWS) ? y : ROWS - 1;
    console_update_cursor();
}

// copy the console cursor position out to *x and *y (neither may be NULL)
void console_get_cursor(uint8_t* x, uint8_t* y) {
    *x = console_x;
    *y = console_y;
}

// blank the screen in the console colours and send the cursor to the top left corner
// unlike clear() this keeps the cursor visible and doesn't count as a full screen takeover
void console_clear(void) {
    vga_fill(VGA_ENTRY(' ', console_fg, console_bg));
    console_x = 0;
    console_y = 0;
    console_update_cursor();
}

// move every line up by one and blank the bottom line
// the copy runs forwards: each cell is copied from one row further down, which has not been
// overwritten yet, so the overlapping source and destination are safe. The top line is lost for
// good, there is no scrollback.
static void console_scroll(void) {
    for (unsigned int i = 0; i < COLS * (ROWS - 1); i++) {
        video[i] = video[i + COLS];
    }
    for (unsigned int x = 0; x < COLS; x++) {
        video[(ROWS - 1) * COLS + x] = VGA_ENTRY(' ', console_fg, console_bg);
    }
}

// the heart of the console: write c at the cursor, handle the control characters, wrap at the end
// of a line, scroll at the bottom of the screen and copy the character to the serial port
// it deliberately does not move the hardware cursor (4 slow port writes), callers do that once
void console_putchar_raw(char c) {
    switch (c) {
        // newline: back to column 0 of the next row (scrolling is handled below)
        case '\n':
            console_x = 0;
            console_y++;
            break;
        // carriage return: back to column 0 of the same row
        case '\r':
            console_x = 0;
            break;
        // backspace: step back one cell, to the end of the previous row if we are in column 0,
        // without erasing anything (console_backspace does the erasing)
        case '\b':
            if (console_x > 0) {
                console_x--;
            } else if (console_y > 0) {
                console_y--;
                console_x = COLS - 1;
            }
            break;
        // tab: pad with spaces up to the next tab stop. Adding 4 then clearing the bottom two bits
        // (& ~3) rounds up to the next multiple of 4, so a tab always moves at least one column
        case '\t': {
            uint8_t next = (uint8_t)((console_x + 4) & ~3);
            while (console_x < next && console_x < COLS) {
                putc(console_x++, console_y, console_fg, console_bg, ' ');
            }
            break;
        }
        // everything else is drawn as a character (putc takes care of the colours)
        default:
            putc(console_x++, console_y, console_fg, console_bg, c);
            break;
    }

    // line full: wrap to the start of the next row
    if (console_x >= COLS) {
        console_x = 0;
        console_y++;
    }
    // went past the bottom row: scroll everything up and stay on the last row
    if (console_y >= ROWS) {
        console_scroll();
        console_y = ROWS - 1;
    }

    // mirror to the serial port for debugging (backspace is mirrored by console_backspace)
    // a serial terminal needs "\r\n" to start a new line, so '\r' is sent before every '\n'
    if (c == '\n') {
        serial_putchar('\r');
    }
    if (c != '\b') {
        serial_putchar(c);
    }
}

// write one character and move the hardware cursor after it (for single characters, e.g. echoing a key)
void console_putchar(char c) {
    console_putchar_raw(c);
    console_update_cursor();
}

// write a whole string, moving the hardware cursor only once at the end instead of per character
void console_write(const char* s) {
    while (*s) {
        console_putchar_raw(*s++);
    }
    console_update_cursor();
}

// erase the character before the cursor: step back, overwrite the cell with a space and leave the
// cursor there. The serial side gets the classic "\b \b" (back, overwrite with a space, back again)
// that terminals use to rub out a character.
void console_backspace(void) {
    console_putchar_raw('\b');
    putc(console_x, console_y, console_fg, console_bg, ' ');
    serial_write("\b \b");
    console_update_cursor();
}

// one-time screen setup, called first thing during boot
// The blink setting lives in the attribute controller, which is awkward: port 0x3C0 is both its
// index and its data port, and an internal flip-flop decides which one the next write goes to.
// Reading the input status register (0x3DA) resets the flip-flop to "index", so after that the
// sequence is: write index to 0x3C0, read the current value from 0x3C1, write the new value to 0x3C0.
// The index 0x30 is register 0x10 (attribute mode control) with bit 5 (0x20) set. Bit 5 is
// "palette address source": if it is clear while we write, the card stops displaying the screen.
// In the mode control register bit 3 (0x08) is "blink enable". With it clear, bit 7 of each cell's
// colour byte stops meaning "blink" and becomes the bright bit of the background colour.
void vga_init(void) {
    // the copyright sign, a circle with a C in it
    static const uint8_t copyright[16] = {
        0x00, 0x00, 0x3C, 0x42, 0x99, 0xA5, 0xA1, 0xA1,
        0xA5, 0x99, 0x42, 0x3C, 0x00, 0x00, 0x00, 0x00
    };

    inb(0x3DA);                // reset the attribute controller flip-flop
    outb(0x3C0, 0x30);         // select the mode control register (bit 5 keeps the screen on)
    uint8_t mode = inb(0x3C1);
    outb(0x3C0, mode & ~0x08); // clear the blink bit
    vga_set_glyph((uint8_t)VGA_COPYRIGHT[0], copyright);
    console_set_color(BRIGHT, BLACK);
    console_clear();
}

// which of the DAC's 256 colours a text colour (0-15) uses
// the attribute controller holds that mapping in its palette registers 0x00 - 0x0F. In a standard
// text mode most map straight across, but brown is 0x14 and the bright colours are 0x38 - 0x3F
// https://wiki.osdev.org/VGA_Hardware#Port_0x3C0
static uint8_t vga_dac_index(enum color color) {
    inb(0x3DA);                              // flip-flop back to "index"
    outb(0x3C0, 0x20 | (color & 0x0F));      // palette register, with bit 5 set to keep the screen on
    uint8_t index = inb(0x3C1);
    inb(0x3DA);                              // reading 0x3C1 left the flip-flop on "data", reset it
    return index;
}

// the DAC takes a register number on 0x3C8 (write) or 0x3C7 (read), then red, green and blue
// one after another through 0x3C9, each 6 bits (0 - 63)
// https://wiki.osdev.org/VGA_Hardware#Port_0x3C8
void vga_set_palette(enum color color, uint8_t r, uint8_t g, uint8_t b) {
    outb(0x3C8, vga_dac_index(color));
    outb(0x3C9, r >> 2);
    outb(0x3C9, g >> 2);
    outb(0x3C9, b >> 2);
}

void vga_get_palette(enum color color, uint8_t* r, uint8_t* g, uint8_t* b) {
    outb(0x3C7, vga_dac_index(color));
    *r = (uint8_t)(inb(0x3C9) << 2);
    *g = (uint8_t)(inb(0x3C9) << 2);
    *b = (uint8_t)(inb(0x3C9) << 2);
}

// read / write one register of the sequencer (0x3C4) or graphics controller (0x3CE):
// the index goes to the port, the value to the port after it
static uint8_t vga_read_reg(uint16_t port, uint8_t index) {
    outb(port, index);
    return inb(port + 1);
}

static void vga_write_reg(uint16_t port, uint8_t index, uint8_t value) {
    outb(port, index);
    outb(port + 1, value);
}

// In text mode the font sits in plane 2 of the VGA's memory, where the CPU normally can't see it
// (0xB8000 only shows planes 0 and 1: the characters and their colours). To reach it we briefly
// point the card's memory window at plane 2 alone, write the glyph, then put everything back.
// Each character has a 32 byte slot in the font, of which the 16 rows of an 8x16 font are used.
// https://wiki.osdev.org/VGA_Fonts
void vga_set_glyph(uint8_t character, const uint8_t* glyph) {
    uint8_t map_mask = vga_read_reg(0x3C4, 0x02);
    uint8_t memory_mode = vga_read_reg(0x3C4, 0x04);
    uint8_t read_map = vga_read_reg(0x3CE, 0x04);
    uint8_t gfx_mode = vga_read_reg(0x3CE, 0x05);
    uint8_t misc = vga_read_reg(0x3CE, 0x06);

    vga_write_reg(0x3C4, 0x02, 0x04); // CPU writes go to plane 2 only
    vga_write_reg(0x3C4, 0x04, 0x06); // plain sequential addressing (odd / even off)
    vga_write_reg(0x3CE, 0x04, 0x02); // CPU reads come from plane 2
    vga_write_reg(0x3CE, 0x05, 0x00); // odd / even off
    vga_write_reg(0x3CE, 0x06, 0x04); // memory window at 0xA0000, 64KB

    volatile uint8_t* slot = (volatile uint8_t*)0xA0000 + character * 32;
    for (int row = 0; row < 16; row++) {
        slot[row] = glyph[row];
    }

    vga_write_reg(0x3C4, 0x02, map_mask);
    vga_write_reg(0x3C4, 0x04, memory_mode);
    vga_write_reg(0x3CE, 0x04, read_map);
    vga_write_reg(0x3CE, 0x05, gfx_mode);
    vga_write_reg(0x3CE, 0x06, misc);
}

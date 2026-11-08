// rendering the screen in the VGA text mode buffer (80x25 characters at 0xB8000)
// https://wiki.osdev.org/Printing_To_Screen
// https://wiki.osdev.org/Text_Mode_Cursor
//
// The screen is always an 80 x 25 grid of cells (see VGA_linear.h for the cell format), shown in
// one of two ways:
//
// - VGA text mode, on PCs started through the BIOS. Drawing is plain memory writes into the cell
//   array at 0xB8000 and the graphics card turns them into characters itself. The blinking cursor
//   and the blink / bright background setting live in registers inside the VGA chip, reached
//   through I/O ports.
//
// - A framebuffer, on PCs started through UEFI. UEFI has no text mode, so GRUB sets up a graphics
//   mode instead and tells us where its pixels are. We keep the same 80 x 25 grid and draw every
//   cell ourselves with the built-in font (font8x16.c), 9 x 16 pixels per cell like real VGA,
//   scaled up by the biggest whole number that fits the screen and centred.
//
// Either way every cell is also kept in cells[], so the framebuffer can be redrawn after a palette
// or font change, and only cells that really change get drawn (which keeps scrolling quick).
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
#include "font8x16.h"
#include "../input/GIO.h"
#include "../serial/serial.h"

// VGA CRT controller ports (cursor control)
// (these are the colour mode addresses, a monochrome card uses 0x3B4 / 0x3B5)
#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5

// the size of one cell in framebuffer pixels (before scaling)
#define CELL_HEIGHT 16

// the text buffer itself, AerOS has no paging so the physical address works as a pointer
volatile uint16_t* const video = (volatile uint16_t*)0xB8000;
volatile unsigned int vga_clear_count = 0;

enum color console_fg = BRIGHT;
enum color console_bg = BLACK;
// the console cursor: the cell the next character goes into (column 0-79, row 0-24)
// this is our own bookkeeping, the hardware cursor is only moved to match it by console_update_cursor
static uint8_t console_x = 0;
static uint8_t console_y = 0;

// whether the cursor is currently switched on, so console_update_cursor only has to re-enable it
// after clear() hid it instead of reprogramming it on every character
static bool vga_cursor_visible = false;
static unsigned int cursor_index = 0; // the cell the cursor is in

// every cell on screen, the framebuffer is drawn from this
static uint16_t cells[COLS * ROWS];

// the framebuffer, when there is one
static bool fb_mode = false;
static struct {
    uint8_t* base;        // first pixel
    uint32_t pitch;       // bytes from one line of pixels to the next
    uint32_t bytes;       // bytes per pixel: 2, 3 or 4
    uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
    uint32_t width, height; // the whole screen in pixels
    uint32_t cell_width;  // 9 like real VGA, or 8 when the screen is too narrow for 720 pixels
    uint32_t scale;       // every font pixel becomes a scale x scale block
    uint32_t left, top;   // where the 80 x 25 grid starts, to centre it
} fb;
static int border_color = BLACK; // the colour of the margin around the grid
static vga_framebuffer_t fb_info; // what GRUB told us, handed on to the graphics module

// the 16 colours as red, green, blue: the standard VGA colours until the palette is changed
static uint8_t palette[16][3] = {
    { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0xAA }, { 0x00, 0xAA, 0x00 }, { 0x00, 0xAA, 0xAA },
    { 0xAA, 0x00, 0x00 }, { 0xAA, 0x00, 0xAA }, { 0xAA, 0x55, 0x00 }, { 0xAA, 0xAA, 0xAA },
    { 0x55, 0x55, 0x55 }, { 0x55, 0x55, 0xFF }, { 0x55, 0xFF, 0x55 }, { 0x55, 0xFF, 0xFF },
    { 0xFF, 0x55, 0x55 }, { 0xFF, 0x55, 0xFF }, { 0xFF, 0xFF, 0x55 }, { 0xFF, 0xFF, 0xFF },
};
static uint32_t fb_colors[16];     // the same colours packed into the framebuffer's pixel format
static uint8_t fb_font[256][16];   // a copy of font8x16, so vga_set_glyph can change it

// framebuffer drawing

// pack a colour into the framebuffer's pixel format, using the bit positions GRUB reported
static uint32_t fb_pack(const uint8_t* rgb) {
    return ((uint32_t)(rgb[0] >> (8 - fb.red_size)) << fb.red_pos) |
           ((uint32_t)(rgb[1] >> (8 - fb.green_size)) << fb.green_pos) |
           ((uint32_t)(rgb[2] >> (8 - fb.blue_size)) << fb.blue_pos);
}

static void fb_put(uint8_t* pixel, uint32_t color) {
    switch (fb.bytes) {
        case 4: *(uint32_t*)pixel = color; break;
        case 3: pixel[0] = color & 0xFF; pixel[1] = (color >> 8) & 0xFF; pixel[2] = (color >> 16) & 0xFF; break;
        default: *(uint16_t*)pixel = (uint16_t)color; break;
    }
}

// draw one cell from cells[], including the underline cursor if it is in this cell
static void fb_draw_cell(unsigned int index) {
    uint8_t ch = cells[index] & 0xFF;
    uint8_t attr = cells[index] >> 8;
    uint32_t fg = fb_colors[attr & 0x0F];
    uint32_t bg = fb_colors[attr >> 4];
    const uint8_t* glyph = fb_font[ch];
    // like real VGA, the box drawing characters 0xC0 - 0xDF repeat their last column in the 9th,
    // so lines join up across cells; everything else gets a blank 9th column
    bool repeat_last = ch >= 0xC0 && ch <= 0xDF;
    bool cursor = vga_cursor_visible && index == cursor_index;

    uint32_t x0 = fb.left + (index % COLS) * fb.cell_width * fb.scale;
    uint32_t y0 = fb.top + (index / COLS) * CELL_HEIGHT * fb.scale;

    for (uint32_t row = 0; row < CELL_HEIGHT; row++) {
        // the cursor is an underline over the last two rows, in the text colour
        uint32_t bits = (cursor && row >= 14) ? 0x1FF : ((uint32_t)glyph[row] << 1);
        if (repeat_last && (glyph[row] & 1)) {
            bits |= 1;
        }
        for (uint32_t sy = 0; sy < fb.scale; sy++) {
            uint8_t* pixel = fb.base + (y0 + row * fb.scale + sy) * fb.pitch + x0 * fb.bytes;
            for (uint32_t col = 0; col < fb.cell_width; col++) {
                uint32_t color = (bits & (0x100 >> col)) ? fg : bg;
                for (uint32_t sx = 0; sx < fb.scale; sx++) {
                    fb_put(pixel, color);
                    pixel += fb.bytes;
                }
            }
        }
    }
}

// paint the margin around the 80 x 25 grid (the screen is rarely an exact multiple of 720 x 400),
// so a full screen layout like the splash fills the whole screen
static void fb_fill_border(int color_index) {
    border_color = color_index;
    uint32_t color = fb_colors[color_index];
    uint32_t right = fb.left + COLS * fb.cell_width * fb.scale;
    uint32_t bottom = fb.top + ROWS * CELL_HEIGHT * fb.scale;
    for (uint32_t y = 0; y < fb.height; y++) {
        bool grid_row = y >= fb.top && y < bottom;
        uint8_t* pixel = fb.base + y * fb.pitch;
        for (uint32_t x = 0; x < fb.width; x++, pixel += fb.bytes) {
            if (grid_row && x == fb.left) {
                // jump over the grid itself
                pixel += (right - x) * fb.bytes;
                x = right;
                if (x >= fb.width) {
                    break;
                }
            }
            fb_put(pixel, color);
        }
    }
}

// redraw every cell that uses colour index (as text or background) or shows character ch
// (pass -1 for whichever one doesn't apply)
static void fb_redraw(int color_index, int ch) {
    for (unsigned int i = 0; i < COLS * ROWS; i++) {
        uint8_t attr = cells[i] >> 8;
        if ((attr & 0x0F) == color_index || (attr >> 4) == color_index || (cells[i] & 0xFF) == ch) {
            fb_draw_cell(i);
        }
    }
}

// take over the framebuffer GRUB set up, returns false if we can't draw on it
static bool fb_setup(const vga_framebuffer_t* info) {
    uint32_t bytes = (info->bpp + 7) / 8;
    if (info->address == 0 || (bytes != 2 && bytes != 3 && bytes != 4)) {
        return false;
    }
    fb_info = *info;
    fb.base = (uint8_t*)(uintptr_t)info->address;
    fb.pitch = info->pitch;
    fb.bytes = bytes;
    fb.red_pos = info->red_pos;
    fb.red_size = info->red_size;
    fb.green_pos = info->green_pos;
    fb.green_size = info->green_size;
    fb.blue_pos = info->blue_pos;
    fb.blue_size = info->blue_size;

    // 720 x 400 like real VGA if it fits, otherwise 8 pixel wide cells (640 x 400)
    fb.cell_width = (info->width >= COLS * 9) ? 9 : 8;
    uint32_t scale_x = info->width / (COLS * fb.cell_width);
    uint32_t scale_y = info->height / (ROWS * CELL_HEIGHT);
    fb.scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (fb.scale == 0) {
        return false; // smaller than 640 x 400
    }
    fb.width = info->width;
    fb.height = info->height;
    fb.left = (info->width - COLS * fb.cell_width * fb.scale) / 2;
    fb.top = (info->height - ROWS * CELL_HEIGHT * fb.scale) / 2;

    for (int i = 0; i < 16; i++) {
        fb_colors[i] = fb_pack(palette[i]);
    }

    // black out the whole screen once, including the border around the grid
    for (uint32_t y = 0; y < info->height; y++) {
        uint8_t* pixel = fb.base + y * fb.pitch;
        for (uint32_t x = 0; x < info->width; x++) {
            fb_put(pixel, fb_colors[BLACK]);
            pixel += fb.bytes;
        }
    }
    return true;
}

bool vga_framebuffer_active(void) {
    return fb_mode;
}

bool vga_framebuffer_info(vga_framebuffer_t* out) {
    if (fb_mode) {
        *out = fb_info;
    }
    return fb_mode;
}

// switch to a new framebuffer (after the graphics card changed mode) and redraw the screen on it
bool vga_change_framebuffer(const vga_framebuffer_t* info) {
    uint32_t bytes = (info->bpp + 7) / 8;
    if (!fb_mode || info->address == 0 || (bytes != 2 && bytes != 3 && bytes != 4) ||
        info->width < COLS * 8 || info->height < ROWS * CELL_HEIGHT) {
        return false;
    }
    fb_setup(info);
    fb_fill_border(border_color);
    for (unsigned int i = 0; i < COLS * ROWS; i++) {
        fb_draw_cell(i);
    }
    return true;
}

void vga_invalidate(void) {
    for (unsigned int i = 0; i < COLS * ROWS; i++) {
        cells[i] = 0xFFFF;
    }
}

// change one cell, on screen and in cells[]
static void cell_set(unsigned int index, uint16_t entry) {
    if (cells[index] == entry) {
        return;
    }
    cells[index] = entry;
    if (fb_mode) {
        fb_draw_cell(index);
    } else {
        video[index] = entry;
    }
}

// cursor

// switch the cursor on and set its shape
// start / end: first and last scanline (0-15 in the 16 pixel tall cell) the cursor covers
// the registers are read first so the bits we don't own are kept, and clearing bit 5 of register
// 0x0A (via the 0xC0 mask) is what actually turns the cursor on
// (the framebuffer cursor is always the underline)
void vga_enable_cursor(uint8_t start, uint8_t end) {
    vga_cursor_visible = true;
    if (fb_mode) {
        fb_draw_cell(cursor_index);
        return;
    }
    outb(VGA_CRTC_INDEX, 0x0A);
    outb(VGA_CRTC_DATA, (inb(VGA_CRTC_DATA) & 0xC0) | start);
    outb(VGA_CRTC_INDEX, 0x0B);
    outb(VGA_CRTC_DATA, (inb(VGA_CRTC_DATA) & 0xE0) | end);
}

// switch the cursor off: 0x20 sets the "cursor disable" bit (bit 5) of register 0x0A
void vga_disable_cursor(void) {
    vga_cursor_visible = false;
    if (fb_mode) {
        fb_draw_cell(cursor_index);
        return;
    }
    outb(VGA_CRTC_INDEX, 0x0A);
    outb(VGA_CRTC_DATA, 0x20);
}

// move the cursor to column x, row y
// the CRTC wants the linear cell index, split over two 8 bit registers (0x0F low, 0x0E high)
void vga_move_cursor(uint8_t x, uint8_t y) {
    unsigned int old = cursor_index;
    cursor_index = (unsigned int)(y * COLS + x);
    if (fb_mode) {
        if (vga_cursor_visible && old != cursor_index) {
            fb_draw_cell(old);
            fb_draw_cell(cursor_index);
        }
        return;
    }
    outb(VGA_CRTC_INDEX, 0x0F);
    outb(VGA_CRTC_DATA, (uint8_t)(cursor_index & 0xFF));
    outb(VGA_CRTC_INDEX, 0x0E);
    outb(VGA_CRTC_DATA, (uint8_t)(cursor_index >> 8));
}

// write the same cell (character + colours) into all 80 * 25 = 2000 cells of the screen
static void vga_fill(uint16_t entry) {
    for (unsigned int i = 0; i < COLS * ROWS; i++) {
        cell_set(i, entry);
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
    cell_set(y * COLS + x, VGA_ENTRY(c, fg, bg));
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
        cell_set(cy * COLS + cx, VGA_ENTRY(*s, fg, bg));
        cx++;
    }
}

// fill the whole screen with colour bg (spaces drawn in bg on bg) and hand the screen over to
// positional drawing
void clear(enum color bg) {
    // full screen layouts (splash, error screens) draw with puts, so hide the console cursor
    vga_disable_cursor();
    vga_fill(VGA_ENTRY(' ', bg, bg));
    if (fb_mode) {
        fb_fill_border(bg);
    }
    console_x = 0;
    console_y = 0;
    // the terminal compares this counter before and after running a command, a change means the
    // command drew its own screen and the terminal needs to redraw itself
    vga_clear_count++;
}

// set the colours the console uses for new characters (and for blank cells when it clears or scrolls)
void console_set_color(enum color fg, enum color bg) {
    console_fg = fg;
    console_bg = bg;
}

// put the blinking cursor where the console cursor is
// if clear() hid it, it is first switched back on as an underline (scanlines 14-15)
void console_update_cursor(void) {
    vga_move_cursor(console_x, console_y);
    if (!vga_cursor_visible) {
        vga_enable_cursor(14, 15);
    }
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
    if (fb_mode) {
        fb_fill_border(console_bg);
    }
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
        cell_set(i, cells[i + COLS]);
    }
    for (unsigned int x = 0; x < COLS; x++) {
        cell_set((ROWS - 1) * COLS + x, VGA_ENTRY(' ', console_fg, console_bg));
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
// In text mode we switch blinking off. The blink setting lives in the attribute controller, which
// is awkward: port 0x3C0 is both its index and its data port, and an internal flip-flop decides
// which one the next write goes to. Reading the input status register (0x3DA) resets the flip-flop
// to "index", so after that the sequence is: write index to 0x3C0, read the current value from
// 0x3C1, write the new value to 0x3C0. The index 0x30 is register 0x10 (attribute mode control)
// with bit 5 (0x20) set. Bit 5 is "palette address source": if it is clear while we write, the
// card stops displaying the screen. In the mode control register bit 3 (0x08) is "blink enable".
// With it clear, bit 7 of each cell's colour byte stops meaning "blink" and becomes the bright bit
// of the background colour.
void vga_init(const vga_framebuffer_t* framebuffer) {
    // the copyright sign, a circle with a C in it
    static const uint8_t copyright[16] = {
        0x00, 0x00, 0x3C, 0x42, 0x99, 0xA5, 0xA1, 0xA1,
        0xA5, 0x99, 0x42, 0x3C, 0x00, 0x00, 0x00, 0x00
    };

    fb_mode = framebuffer != 0 && fb_setup(framebuffer);
    if (fb_mode) {
        for (int ch = 0; ch < 256; ch++) {
            for (int row = 0; row < CELL_HEIGHT; row++) {
                fb_font[ch][row] = font8x16[ch][row];
            }
        }
    } else {
        inb(0x3DA);                // reset the attribute controller flip-flop
        outb(0x3C0, 0x30);         // select the mode control register (bit 5 keeps the screen on)
        uint8_t mode = inb(0x3C1);
        outb(0x3C0, mode & ~0x08); // clear the blink bit
    }
    vga_set_glyph((uint8_t)VGA_COPYRIGHT[0], copyright);

    // cells[] starts out not matching anything on screen, so the first clear draws every cell
    for (unsigned int i = 0; i < COLS * ROWS; i++) {
        cells[i] = 0xFFFF;
    }
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
    if (fb_mode) {
        palette[color][0] = r;
        palette[color][1] = g;
        palette[color][2] = b;
        fb_colors[color] = fb_pack(palette[color]);
        fb_redraw(color, -1);
        if (border_color == (int)color) {
            fb_fill_border(color);
        }
        return;
    }
    outb(0x3C8, vga_dac_index(color));
    outb(0x3C9, r >> 2);
    outb(0x3C9, g >> 2);
    outb(0x3C9, b >> 2);
}

void vga_get_palette(enum color color, uint8_t* r, uint8_t* g, uint8_t* b) {
    if (fb_mode) {
        *r = palette[color][0];
        *g = palette[color][1];
        *b = palette[color][2];
        return;
    }
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
// In framebuffer mode the font is our own copy, so the glyph just replaces it there.
// https://wiki.osdev.org/VGA_Fonts
void vga_set_glyph(uint8_t character, const uint8_t* glyph) {
    if (fb_mode) {
        for (int row = 0; row < CELL_HEIGHT; row++) {
            fb_font[character][row] = glyph[row];
        }
        fb_redraw(-1, character);
        return;
    }

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

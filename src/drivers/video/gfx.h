/*
    gfx.h
    Graphics for apps: pixels, rectangles, lines, text and BMP pictures, drawn straight into the
    framebuffer. Colours are 0xRRGGBB. Everything is clipped to the screen, so drawing partly
    (or completely) off screen is fine.

    An app calls gfx_begin() to take the screen over from the terminal and gfx_end() to give it
    back. Graphics need a framebuffer, which every PC started through GRUB has (VGA text mode
    is only left when there is no framebuffer at all).
*/
#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include <stdbool.h>

// take over the screen (cleared to black), false if there is no framebuffer to draw on
bool gfx_begin(void);

// hand the screen back to the terminal, which redraws itself
void gfx_end(void);

uint32_t gfx_width(void);
uint32_t gfx_height(void);

void gfx_clear(uint32_t rgb);
void gfx_pixel(int x, int y, uint32_t rgb);
void gfx_rect(int x, int y, int w, int h, uint32_t rgb);       // filled
void gfx_line(int x0, int y0, int x1, int y1, uint32_t rgb);
// text with the built-in 8x16 font, each font pixel drawn as a scale x scale square
void gfx_text(int x, int y, const char* text, uint32_t rgb, int scale);

// the size of a BMP picture, false if the data isn't a BMP we can show (8, 24 or 32 bit, uncompressed)
bool gfx_image_size(const uint8_t* data, uint32_t length, int* w, int* h);

// draw a BMP picture with its top left corner at x, y. If max_w and max_h are above 0 the picture
// is scaled (up or down, keeping its shape) to fit in that box. The box may be bigger than the
// screen (up to 16384 pixels each way) and x, y may be negative, which is how a picture is zoomed
// in on: only the part that lands on the screen is drawn. False if it isn't a BMP we can show.
bool gfx_image(const uint8_t* data, uint32_t length, int x, int y, int max_w, int max_h);

#endif /* GFX_H */

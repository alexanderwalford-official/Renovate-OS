/*
    gfx.c - graphics for apps

    Draws straight into the framebuffer GRUB set up (see VGA_linear.c), one pixel at a time.
    A pixel is 2, 3 or 4 bytes, with red, green and blue at the bit positions GRUB reported.

    BMP pictures: a 14 byte file header ("BM", where the pixels start), a 40+ byte info header
    (width, height, bits per pixel, compression), an optional palette, then the rows of pixels.
    Rows are stored bottom up unless the height is negative, each one padded to a multiple of
    4 bytes, with pixels in blue, green, red order.
    https://en.wikipedia.org/wiki/BMP_file_format
*/

#include "gfx.h"
#include "VGA_linear.h"
#include "font8x16.h"

static vga_framebuffer_t fb; // where the framebuffer is and how its pixels are laid out
static uint32_t fb_bytes;    // bytes per pixel
static bool active = false;  // between gfx_begin and gfx_end, every drawing call checks it

// take the screen over from the text console (false in VGA text mode, there are no pixels)
bool gfx_begin(void) {
    if (!vga_framebuffer_info(&fb)) {
        return false;
    }
    fb_bytes = (fb.bpp + 7) / 8;
    clear(BLACK); // hides the cursor, and tells the terminal to redraw itself afterwards
    vga_invalidate();
    active = true;
    gfx_clear(0x000000);
    return true;
}

// give the screen back: the console redraws all of its text the next time it is used
void gfx_end(void) {
    if (!active) {
        return;
    }
    active = false;
    vga_invalidate();
    clear(BLACK);
}

uint32_t gfx_width(void) {
    return active ? fb.width : 0;
}

uint32_t gfx_height(void) {
    return active ? fb.height : 0;
}

// turn 0xRRGGBB into the framebuffer's own pixel format. Each colour is cut down to however
// many bits the screen has for it and moved to where the screen keeps it, e.g.
//     32 bit (8:8:8)   red at bit 16, green at 8, blue at 0      -> 0x00RRGGBB
//     16 bit (5:6:5)   red at bit 11, green at 5, blue at 0      -> rrrrrggggggbbbbb
static uint32_t pack(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return ((r >> (8 - fb.red_size)) << fb.red_pos) |
           ((g >> (8 - fb.green_size)) << fb.green_pos) |
           ((b >> (8 - fb.blue_size)) << fb.blue_pos);
}

// write one packed pixel (the caller has checked x and y are on screen)
static void put(uint32_t x, uint32_t y, uint32_t pixel) {
    uint8_t* p = (uint8_t*)(uintptr_t)fb.address + y * fb.pitch + x * fb_bytes;
    switch (fb_bytes) {
        case 4: *(uint32_t*)p = pixel; break;
        case 3: p[0] = pixel & 0xFF; p[1] = (pixel >> 8) & 0xFF; p[2] = (pixel >> 16) & 0xFF; break;
        default: *(uint16_t*)p = (uint16_t)pixel; break;
    }
}

// every public drawing call clips to the screen, so apps can draw partly (or wholly) off it
void gfx_pixel(int x, int y, uint32_t rgb) {
    if (active && x >= 0 && y >= 0 && (uint32_t)x < fb.width && (uint32_t)y < fb.height) {
        put((uint32_t)x, (uint32_t)y, pack(rgb));
    }
}

void gfx_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (!active) {
        return;
    }
    // Clip to the screen. A rectangle starting off the left (or top) edge loses the part that is
    // off screen: e.g. x = -10, w = 50 becomes x = 0, w = 40
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (w <= 0 || h <= 0 || (uint32_t)x >= fb.width || (uint32_t)y >= fb.height) {
        return;
    }
    if ((uint32_t)(x + w) > fb.width) {
        w = (int)fb.width - x;
    }
    if ((uint32_t)(y + h) > fb.height) {
        h = (int)fb.height - y;
    }

    uint32_t pixel = pack(rgb);
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            put((uint32_t)(x + col), (uint32_t)(y + row), pixel);
        }
    }
}

void gfx_clear(uint32_t rgb) {
    gfx_rect(0, 0, (int)fb.width, (int)fb.height, rgb);
}

// a number without its sign: 5 for both 5 and -5
static int distance (int a, int b) {
    if (a > b) {
        return a - b;
    }
    return b - a;
}

// which way to step to get from a to b: +1, or -1
static int direction (int from, int to) {
    if (from < to) {
        return 1;
    }
    return -1;
}

// Draw a line with Bresenham's method, using only whole numbers.
// It moves one pixel at a time from (x0, y0) towards (x1, y1). `error` keeps track of how far
// the pixels drawn so far have drifted from the true line; whenever that drift gets to half a
// pixel in a direction, the next step goes that way too. A shallow line steps sideways every
// time and only sometimes up or down; a steep line the other way round
void gfx_line(int x0, int y0, int x1, int y1, uint32_t rgb) {
    int width = distance(x0, x1);
    int height = -distance(y0, y1); // negative: the maths below is neatest this way round
    int step_x = direction(x0, x1);
    int step_y = direction(y0, y1);
    int error = width + height;

    for (;;) {
        gfx_pixel(x0, y0, rgb);
        if (x0 == x1 && y0 == y1) {
            break; // reached the end
        }
        int doubled = 2 * error;
        if (doubled >= height) {
            // drifted far enough to take a step sideways
            error += height;
            x0 += step_x;
        }
        if (doubled <= width) {
            // drifted far enough to take a step up or down
            error += width;
            y0 += step_y;
        }
    }
}

// text in the console's 8 x 16 font, each font pixel drawn as a scale x scale square
void gfx_text(int x, int y, const char* text, uint32_t rgb, int scale) {
    if (scale < 1) {
        scale = 1;
    }
    for (; *text; text++, x += 8 * scale) {
        const uint8_t* glyph = font8x16[(uint8_t)*text];
        for (int row = 0; row < 16; row++) {
            for (int col = 0; col < 8; col++) {
                if (glyph[row] & (0x80 >> col)) {
                    gfx_rect(x + col * scale, y + row * scale, scale, scale, rgb);
                }
            }
        }
    }
}

// ---- BMP pictures -----------------------------------------------------------------------------
//
// The header fields read here (all little endian):
//     offset  0  "BM"                      offset 22  height (negative = rows stored top down)
//     offset 10  where the pixels start    offset 28  bits per pixel (8, 24 or 32 here)
//     offset 14  info header size (40+)    offset 30  compression (0, or 3 for 32 bit)
//     offset 18  width                     offset 46  palette entries (8 bit, 0 means 256)
// The palette (8 bit pictures only) follows the info header, 4 bytes per colour: blue, green,
// red, unused.

// a picture checked by bmp_open, with everything needed to find any pixel
typedef struct {
    const uint8_t* data;
    int width;
    int height;
    bool top_down;
    uint32_t bpp;
    uint32_t pixels;  // where the pixel rows start
    uint32_t stride;  // bytes per row, including the padding
    const uint8_t* palette;
    uint32_t colours; // entries in the palette
} bmp_t;

static uint32_t u16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t u32(const uint8_t* p) { return u16(p) | (u16(p + 2) << 16); }

// read and check the headers, false for anything that isn't a BMP we can draw, or that claims
// more pixels than the file holds (so a damaged file can't make us read past its end)
static bool bmp_open(const uint8_t* d, uint32_t length, bmp_t* b) {
    if (d == 0 || length < 54 || d[0] != 'B' || d[1] != 'M') {
        return false;
    }
    uint32_t header = u32(d + 14);
    int32_t width = (int32_t)u32(d + 18);
    int32_t height = (int32_t)u32(d + 22);
    uint32_t compression = u32(d + 30);
    b->data = d;
    b->bpp = u16(d + 28);
    b->pixels = u32(d + 10);
    b->top_down = height < 0;
    b->width = width;
    b->height = height < 0 ? -height : height;

    if (header < 40 || width <= 0 || width > 16384 || b->height == 0 || b->height > 16384) {
        return false;
    }
    if (b->bpp != 8 && b->bpp != 24 && b->bpp != 32) {
        return false;
    }
    // 0 = plain, 3 = "bitfields", which for 32 bit pictures is the usual blue, green, red, alpha order
    if (compression != 0 && !(compression == 3 && b->bpp == 32)) {
        return false;
    }

    // each row is padded to a whole number of 4 byte words
    b->stride = (((uint32_t)width * b->bpp + 31) / 32) * 4;
    if ((uint64_t)b->pixels + (uint64_t)b->stride * (uint32_t)b->height > length) {
        return false;
    }

    if (b->bpp == 8) {
        b->colours = u32(d + 46) ? u32(d + 46) : 256;
        b->palette = d + 14 + header;
        if (b->colours > 256 || 14 + header + b->colours * 4 > length) {
            return false;
        }
    }
    return true;
}

// the colour of one pixel as 0xRRGGBB, (0, 0) being the top left
static uint32_t bmp_pixel(const bmp_t* b, int x, int y) {
    int row = b->top_down ? y : b->height - 1 - y;
    const uint8_t* p = b->data + b->pixels + (uint32_t)row * b->stride;
    if (b->bpp == 8) {
        uint32_t index = p[x];
        if (index >= b->colours) {
            return 0;
        }
        p = b->palette + index * 4;
    } else {
        p += (uint32_t)x * (b->bpp / 8);
    }
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

bool gfx_image_size(const uint8_t* data, uint32_t length, int* w, int* h) {
    bmp_t b;
    if (!bmp_open(data, length, &b)) {
        return false;
    }
    *w = b.width;
    *h = b.height;
    return true;
}

bool gfx_image(const uint8_t* data, uint32_t length, int x, int y, int max_w, int max_h) {
    bmp_t b;
    if (!active || !bmp_open(data, length, &b)) {
        return false;
    }

    // the size to draw at: as it is, or as big as fits in the box while keeping its shape.
    // The box can be far bigger than the screen (a zoomed in picture, only part of which shows).
    // Pictures and boxes are both at most 16384 pixels across, so every product below stays under
    // 2^28 and fits in 32 bits (64 bit division would need libgcc, which the kernel doesn't link)
    uint32_t w = (uint32_t)b.width;
    uint32_t h = (uint32_t)b.height;
    if (max_w > 0 && max_h > 0) {
        if (max_w > 16384) {
            max_w = 16384;
        }
        if (max_h > 16384) {
            max_h = 16384;
        }
        if ((uint32_t)b.width * (uint32_t)max_h <= (uint32_t)b.height * (uint32_t)max_w) {
            h = (uint32_t)max_h;
            w = (uint32_t)b.width * (uint32_t)max_h / (uint32_t)b.height;
        } else {
            w = (uint32_t)max_w;
            h = (uint32_t)b.height * (uint32_t)max_w / (uint32_t)b.width;
        }
        if (w < 1) {
            w = 1;
        }
        if (h < 1) {
            h = 1;
        }
    }

    // only the part of the picture that lands on the screen is worth drawing: tx and ty run over
    // the drawn picture, so start where it comes onto the screen and stop where it goes off.
    // A zoomed in picture can be thousands of pixels wider than the screen
    uint32_t x0 = x < 0 ? (uint32_t)-x : 0, x1 = w;
    uint32_t y0 = y < 0 ? (uint32_t)-y : 0, y1 = h;
    if (x >= (int)fb.width || y >= (int)fb.height) {
        return true;
    }
    if (x + (int)w > (int)fb.width) {
        x1 = (uint32_t)((int)fb.width - x);
    }
    if (y + (int)h > (int)fb.height) {
        y1 = (uint32_t)((int)fb.height - y);
    }

    // nearest neighbour: every screen pixel takes the colour of the picture pixel under it
    for (uint32_t ty = y0; ty < y1; ty++) {
        int py = (int)(ty * (uint32_t)b.height / h);
        for (uint32_t tx = x0; tx < x1; tx++) {
            int px = (int)(tx * (uint32_t)b.width / w);
            put((uint32_t)(x + (int)tx), (uint32_t)(y + (int)ty), pack(bmp_pixel(&b, px, py)));
        }
    }
    return true;
}

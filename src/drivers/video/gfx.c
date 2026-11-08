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

static vga_framebuffer_t fb;
static uint32_t fb_bytes; // bytes per pixel
static bool active = false;

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

void gfx_pixel(int x, int y, uint32_t rgb) {
    if (active && x >= 0 && y >= 0 && (uint32_t)x < fb.width && (uint32_t)y < fb.height) {
        put((uint32_t)x, (uint32_t)y, pack(rgb));
    }
}

void gfx_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (!active) return;
    // clip to the screen
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0 || (uint32_t)x >= fb.width || (uint32_t)y >= fb.height) return;
    if ((uint32_t)(x + w) > fb.width) w = (int)fb.width - x;
    if ((uint32_t)(y + h) > fb.height) h = (int)fb.height - y;

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

// Bresenham's line: step along the longer direction one pixel at a time, and step along the
// shorter one whenever the accumulated error says the line has drifted half a pixel
void gfx_line(int x0, int y0, int x1, int y1, uint32_t rgb) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0; // negative
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        gfx_pixel(x0, y0, rgb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * error;
        if (e2 >= dy) { error += dy; x0 += sx; }
        if (e2 <= dx) { error += dx; y0 += sy; }
    }
}

void gfx_text(int x, int y, const char* text, uint32_t rgb, int scale) {
    if (scale < 1) scale = 1;
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

// BMP pictures

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

static bool bmp_open(const uint8_t* d, uint32_t length, bmp_t* b) {
    if (d == 0 || length < 54 || d[0] != 'B' || d[1] != 'M') return false;
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

    if (header < 40 || width <= 0 || width > 16384 || b->height == 0 || b->height > 16384) return false;
    if (b->bpp != 8 && b->bpp != 24 && b->bpp != 32) return false;
    // 0 = plain, 3 = "bitfields", which for 32 bit pictures is the usual blue, green, red, alpha order
    if (compression != 0 && !(compression == 3 && b->bpp == 32)) return false;

    b->stride = (((uint32_t)width * b->bpp + 31) / 32) * 4;
    if ((uint64_t)b->pixels + (uint64_t)b->stride * (uint32_t)b->height > length) return false;

    if (b->bpp == 8) {
        b->colours = u32(d + 46) ? u32(d + 46) : 256;
        b->palette = d + 14 + header;
        if (b->colours > 256 || 14 + header + b->colours * 4 > length) return false;
    }
    return true;
}

// the colour of one pixel as 0xRRGGBB, (0, 0) being the top left
static uint32_t bmp_pixel(const bmp_t* b, int x, int y) {
    int row = b->top_down ? y : b->height - 1 - y;
    const uint8_t* p = b->data + b->pixels + (uint32_t)row * b->stride;
    if (b->bpp == 8) {
        uint32_t index = p[x];
        if (index >= b->colours) return 0;
        p = b->palette + index * 4;
    } else {
        p += (uint32_t)x * (b->bpp / 8);
    }
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

bool gfx_image_size(const uint8_t* data, uint32_t length, int* w, int* h) {
    bmp_t b;
    if (!bmp_open(data, length, &b)) return false;
    *w = b.width;
    *h = b.height;
    return true;
}

bool gfx_image(const uint8_t* data, uint32_t length, int x, int y, int max_w, int max_h) {
    bmp_t b;
    if (!active || !bmp_open(data, length, &b)) return false;

    // the size to draw at: as it is, or as big as fits in the box while keeping its shape
    // (pictures are at most 16384 pixels across and the box is screen sized, so these products
    // fit easily in 32 bits; 64 bit division would need libgcc, which the kernel doesn't link)
    uint32_t w = (uint32_t)b.width;
    uint32_t h = (uint32_t)b.height;
    if (max_w > 0 && max_h > 0) {
        if (max_w > 8192) max_w = 8192;
        if (max_h > 8192) max_h = 8192;
        if ((uint32_t)b.width * (uint32_t)max_h <= (uint32_t)b.height * (uint32_t)max_w) {
            h = (uint32_t)max_h;
            w = (uint32_t)b.width * (uint32_t)max_h / (uint32_t)b.height;
        } else {
            w = (uint32_t)max_w;
            h = (uint32_t)b.height * (uint32_t)max_w / (uint32_t)b.width;
        }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
    }

    // nearest neighbour: every screen pixel takes the colour of the picture pixel under it
    for (uint32_t ty = 0; ty < h; ty++) {
        int sy = y + (int)ty;
        if (sy < 0 || (uint32_t)sy >= fb.height) continue;
        int py = (int)(ty * (uint32_t)b.height / h);
        for (uint32_t tx = 0; tx < w; tx++) {
            int sx = x + (int)tx;
            if (sx < 0 || (uint32_t)sx >= fb.width) continue;
            int px = (int)(tx * (uint32_t)b.width / w);
            put((uint32_t)sx, (uint32_t)sy, pack(bmp_pixel(&b, px, py)));
        }
    }
    return true;
}

/*
    gfx.c - graphics for apps

    Draws into the framebuffer GRUB set up (see VGA_linear.c). A pixel is 2, 3 or 4 bytes, with
    red, green and blue at the bit positions GRUB reported.

    DRAWING STRAIGHT TO THE SCREEN, OR TO A BACK BUFFER

    By default everything is drawn straight onto the screen, so you see each step as it
    happens. That is fine for a still picture, but anything that moves flickers: the screen
    shows the frame half cleared and half redrawn.

    gfx_buffered(true) gives a "back buffer" instead: a copy of the screen in memory, in exactly
    the screen's own pixel format. Everything is drawn there, out of sight, and gfx_show()
    copies the finished frame onto the screen in one go, a row at a time with memcpy. The
    screen only ever shows whole frames. Every drawing function writes through `target`, which
    is either the screen or the back buffer:

        drawing --> target --(straight)----------------------------> screen
                          \--(buffered)--> back buffer --gfx_show()--> screen

    BMP pictures: a 14 byte file header ("BM", where the pixels start), a 40+ byte info header
    (width, height, bits per pixel, compression), an optional palette, then the rows of pixels.
    Rows are stored bottom up unless the height is negative, each one padded to a multiple of
    4 bytes, with pixels in blue, green, red order.
    https://en.wikipedia.org/wiki/BMP_file_format
*/

#include "gfx.h"
#include "VGA_linear.h"
#include "font8x16.h"
#include "../../lib/aer_stdlib.h"
#include "../../lib/aer_string.h"

static vga_framebuffer_t fb; // where the framebuffer is and how its pixels are laid out
static uint32_t fb_bytes;    // bytes per pixel
static bool active = false;  // between gfx_begin and gfx_end, every drawing call checks it

// where drawing goes: the screen itself, or the back buffer (see the top of this file)
static uint8_t* target;        // the first pixel
static uint32_t target_pitch;  // bytes from one row of pixels to the next
static uint8_t* back = NULL;   // the back buffer, NULL when drawing straight to the screen

// take the screen over from the text console (false in VGA text mode, there are no pixels)
bool gfx_begin(void) {
    if (!vga_framebuffer_info(&fb)) {
        return false;
    }
    fb_bytes = (fb.bpp + 7) / 8; // bits rounded up to whole bytes (15 bit colour takes 2)
    // drawing starts out going straight to the screen
    target = (uint8_t*)(uintptr_t)fb.address;
    target_pitch = fb.pitch;
    clear(BLACK); // hides the cursor, and tells the terminal to redraw itself afterwards
    // the console's cells[] no longer match the pixels once we draw over them
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
    gfx_buffered(false); // shows the last frame and frees the back buffer, if there is one
    active = false;
    // every cell counts as changed, so the clear below repaints the whole screen over our pixels
    vga_invalidate();
    clear(BLACK);
}

// switch between drawing straight to the screen and drawing to a back buffer (see the top of
// this file), false if there are no graphics or no memory for the buffer
bool gfx_buffered(bool on) {
    if (!active) {
        return false;
    }
    uint8_t* screen = (uint8_t*)(uintptr_t)fb.address;
    // the back buffer's rows are exactly as long as the pixels in them, with none of the
    // padding the screen's rows may have (fb.pitch)
    uint32_t row_bytes = fb.width * fb_bytes;

    // switching on (asking twice does nothing)
    if (on && back == NULL) {
        back = malloc((size_t)row_bytes * fb.height);
        if (back == NULL) {
            return false; // not enough memory: carry on drawing straight to the screen
        }
        // start from what is on the screen now, so nothing drawn so far is lost
        for (uint32_t y = 0; y < fb.height; y++) {
            memcpy(back + y * row_bytes, screen + y * fb.pitch, row_bytes);
        }
        target = back;
        target_pitch = row_bytes;
    } else if (!on && back != NULL) {
        // switching off: drawing goes straight to the screen again
        gfx_show(); // the screen gets the last frame drawn
        free(back);
        back = NULL;
        target = screen;
        target_pitch = fb.pitch;
    }
    return true;
}

// copy the finished frame from the back buffer onto the screen
void gfx_show(void) {
    if (!active || back == NULL) {
        return; // drawing straight to the screen: it is already there
    }
    uint8_t* screen = (uint8_t*)(uintptr_t)fb.address;
    uint32_t row_bytes = fb.width * fb_bytes;
    // a row at a time, because the screen's rows may be longer than the picture (fb.pitch is
    // sometimes rounded up), while the back buffer's rows are exactly row_bytes long
    for (uint32_t y = 0; y < fb.height; y++) {
        memcpy(screen + y * fb.pitch, back + y * row_bytes, row_bytes);
    }
}

// the screen's width in pixels, 0 when graphics aren't on
uint32_t gfx_width(void) {
    return active ? fb.width : 0;
}

// the screen's height in pixels, 0 when graphics aren't on
uint32_t gfx_height(void) {
    return active ? fb.height : 0;
}

// turn 0xRRGGBB into the framebuffer's own pixel format. Each colour is cut down to however
// many bits the screen has for it and moved to where the screen keeps it, e.g.
//     32 bit (8:8:8)   red at bit 16, green at 8, blue at 0      -> 0x00RRGGBB
//     16 bit (5:6:5)   red at bit 11, green at 5, blue at 0      -> rrrrrggggggbbbbb
static uint32_t pack(uint32_t rgb) {
    // split 0xRRGGBB into its three bytes
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    // shifting right by 8 - size keeps the top `size` bits, the shift left puts them in place
    return ((r >> (8 - fb.red_size)) << fb.red_pos) |
           ((g >> (8 - fb.green_size)) << fb.green_pos) |
           ((b >> (8 - fb.blue_size)) << fb.blue_pos);
}

// write one packed pixel (the caller has checked x and y are on screen)
static void put(uint32_t x, uint32_t y, uint32_t pixel) {
    // the pixel's address: y whole rows down (pitch bytes each), then x pixels across
    uint8_t* p = target + y * target_pitch + x * fb_bytes;
    // there is no 24 bit type, so a 3 byte pixel goes a byte at a time, lowest byte first
    switch (fb_bytes) {
        case 4:
            *(uint32_t*)p = pixel;
            break;
        case 3:
            p[0] = pixel & 0xFF;
            p[1] = (pixel >> 8) & 0xFF;
            p[2] = (pixel >> 16) & 0xFF;
            break;
        default:
            *(uint16_t*)p = (uint16_t)pixel;
            break;
    }
}

// fill `count` pixels of one row, starting at x, y, with a packed pixel. 32 bit screens (the
// usual kind) are filled 4 bytes a step with "rep stosl" (see memset in lib/aer_string.c),
// which makes clearing the whole screen every frame cheap
static void fill_row(uint32_t x, uint32_t y, uint32_t count, uint32_t pixel) {
    if (fb_bytes == 4) {
        void* p = target + y * target_pitch + x * 4;
        // rep stosl stores EAX at [EDI] ECX times, moving EDI on 4 bytes each time. "+D" and
        // "+c" tell the compiler both registers change, "memory" that the pixels do too
        __asm__ volatile ("rep stosl" : "+D"(p), "+c"(count) : "a"(pixel) : "memory");
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        put(x + i, y, pixel);
    }
}

// one pixel at x, y in the colour 0xRRGGBB
// every public drawing call clips to the screen, so apps can draw partly (or wholly) off it
void gfx_pixel(int x, int y, uint32_t rgb) {
    if (active && x >= 0 && y >= 0 && (uint32_t)x < fb.width && (uint32_t)y < fb.height) {
        put((uint32_t)x, (uint32_t)y, pack(rgb));
    }
}

// a filled rectangle, w x h pixels with its top left corner at x, y
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
    // nothing left of it, or it starts past the right or bottom edge
    if (w <= 0 || h <= 0 || (uint32_t)x >= fb.width || (uint32_t)y >= fb.height) {
        return;
    }
    // and cut off whatever hangs over the right and bottom edges
    if ((uint32_t)(x + w) > fb.width) {
        w = (int)fb.width - x;
    }
    if ((uint32_t)(y + h) > fb.height) {
        h = (int)fb.height - y;
    }

    uint32_t pixel = pack(rgb); // once, not for every pixel
    for (int row = 0; row < h; row++) {
        fill_row((uint32_t)x, (uint32_t)(y + row), (uint32_t)w, pixel);
    }
}

// fill the whole screen with one colour
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
        // one byte per row of the letter, bit 7 the leftmost pixel. Only the set bits are drawn,
        // so the text has no background and whatever is behind it shows through
        for (int row = 0; row < 16; row++) {
            for (int col = 0; col < 8; col++) {
                if (glyph[row] & (0x80 >> col)) {
                    gfx_rect(x + col * scale, y + row * scale, scale, scale, rgb);
                }
            }
        }
    }
}

// ---- drawing a block of pixels, scaled (video frames, sprites) -------------------------------
//
// Nearest neighbour scaling: every pixel drawn takes the colour of the source pixel under it.
// Two things make it quick enough for 25 or 30 video frames a second:
//
//   1. Which source column each column on the screen comes from is worked out once per call
//      (source_column[]), instead of a division for every pixel.
//   2. When the picture is made bigger, neighbouring screen rows often come from the same source
//      row. Then the row just drawn is copied with memcpy instead of being worked out again.
//
// On the usual kind of screen (32 bits a pixel, red-green-blue in the same order as 0x00RRGGBB)
// a source pixel can be written straight to the screen; any other kind goes through pack().

#define MOST_COLUMNS 16384
static uint32_t source_column[MOST_COLUMNS];

// is the screen 32 bit 0x00RRGGBB, the same layout as the pixels handed in?
static bool screen_is_xrgb (void) {
    return fb_bytes == 4 && fb.red_pos == 16 && fb.green_pos == 8 && fb.blue_pos == 0 &&
           fb.red_size == 8 && fb.green_size == 8 && fb.blue_size == 8;
}

// draw a w x h block of 0x00RRGGBB pixels scaled to out_w x out_h at x, y (see above)
void gfx_draw_pixels(const uint32_t* pixels, int w, int h, int stride, int x, int y, int out_w, int out_h) {
    if (!active || pixels == NULL || w <= 0 || h <= 0 || out_w <= 0 || out_h <= 0) {
        return;
    }
    // source_column[] only has room for this many (far wider than any screen)
    if (out_w > MOST_COLUMNS) {
        out_w = MOST_COLUMNS;
    }

    // the part of the scaled picture that lands on the screen, in picture coordinates
    // (column 0 being the picture's left edge on the screen, wherever that is)
    int first_column = 0;
    int last_column = out_w;      // one past the last
    int first_row = 0;
    int last_row = out_h;
    if (x < 0) {
        first_column = -x;
    }
    if (y < 0) {
        first_row = -y;
    }
    if (x + out_w > (int)fb.width) {
        last_column = (int)fb.width - x;
    }
    if (y + out_h > (int)fb.height) {
        last_row = (int)fb.height - y;
    }
    if (first_column >= last_column || first_row >= last_row) {
        return; // all of it is off the screen
    }

    // screen column `column` of out_w shows source column column * w / out_w of w: the same
    // fraction of the way across. Multiplying first keeps it exact, e.g. drawing 320 pixels at
    // 960 wide gives columns 0, 0, 0, 1, 1, 1, 2...
    for (int column = first_column; column < last_column; column++) {
        source_column[column] = (uint32_t)column * (uint32_t)w / (uint32_t)out_w;
    }

    bool straight = screen_is_xrgb();
    // the bytes of one drawn row on the screen, for copying a row that repeats
    uint32_t row_bytes = (uint32_t)(last_column - first_column) * fb_bytes;
    int previous_source_row = -1; // no row drawn yet, and -1 matches no real source row
    uint8_t* previous_row = NULL;

    for (int row = first_row; row < last_row; row++) {
        // the source row, worked out the same way as the columns
        int source_row = (int)((uint32_t)row * (uint32_t)h / (uint32_t)out_h);
        // where this row starts on the screen (or in the back buffer): its line, then the first
        // column that is on the screen
        uint8_t* screen_row = target + (uint32_t)(y + row) * target_pitch + (uint32_t)(x + first_column) * fb_bytes;

        if (source_row == previous_source_row) {
            memcpy(screen_row, previous_row, row_bytes); // the same as the row above
            continue;
        }

        // a new source row: pick out its pixels column by column
        const uint32_t* source = pixels + source_row * stride;
        if (straight) {
            // the screen's own format, so each pixel is copied as it is (the top byte is
            // ignored by the screen)
            uint32_t* out = (uint32_t*)screen_row;
            for (int column = first_column; column < last_column; column++) {
                *out++ = source[source_column[column]];
            }
        } else {
            // any other format: repacked a pixel at a time (slower, but rare)
            for (int column = first_column; column < last_column; column++) {
                uint32_t pixel = pack(source[source_column[column]]);
                put((uint32_t)(x + column), (uint32_t)(y + row), pixel);
            }
        }
        previous_source_row = source_row;
        previous_row = screen_row;
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

// a 16 bit little endian number at p: the first byte is the low one. Read a byte at a time, as
// BMP fields aren't always lined up on a 2 or 4 byte boundary
static uint32_t u16(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

// a 32 bit little endian number at p: two 16 bit halves, the low half first
static uint32_t u32(const uint8_t* p) {
    return u16(p) | (u16(p + 2) << 16);
}

// read and check the headers, false for anything that isn't a BMP we can draw, or that claims
// more pixels than the file holds (so a damaged file can't make us read past its end)
static bool bmp_open(const uint8_t* d, uint32_t length, bmp_t* b) {
    // 54 bytes: the 14 byte file header and the smallest (40 byte) info header
    if (d == 0 || length < 54 || d[0] != 'B' || d[1] != 'M') {
        return false;
    }
    // the fields (see the table above). Width and height are signed, as a negative height
    // means the rows are stored top down
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

    // older (12 byte) headers are a different layout, and anything over 16384 pixels is more
    // likely a damaged file than a picture (it also keeps the sums in gfx_image within 32 bits)
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

    // each row is padded to a whole number of 4 byte words: the bits in a row, rounded up to
    // a multiple of 32 (adding 31 before dividing), as bytes. e.g. 3 pixels at 24 bits = 72 bits,
    // which rounds up to 96 bits = 12 bytes
    b->stride = (((uint32_t)width * b->bpp + 31) / 32) * 4;
    // all the rows must be in the file. Worked out in 64 bits, as a damaged header could make
    // the product wrap round in 32 bits and look small (multiplying is fine, it's only 64 bit
    // division that needs libgcc)
    if ((uint64_t)b->pixels + (uint64_t)b->stride * (uint32_t)b->height > length) {
        return false;
    }

    // an 8 bit picture's pixels are palette numbers: find the palette, and check it is all there
    if (b->bpp == 8) {
        b->colours = u32(d + 46) ? u32(d + 46) : 256;
        b->palette = d + 14 + header; // straight after the info header, whatever size that is
        if (b->colours > 256 || 14 + header + b->colours * 4 > length) {
            return false;
        }
    }
    return true;
}

// the colour of one pixel as 0xRRGGBB, (0, 0) being the top left
static uint32_t bmp_pixel(const bmp_t* b, int x, int y) {
    // most BMPs store the bottom row first, so row y from the top is that many rows from the end
    int row = b->top_down ? y : b->height - 1 - y;
    const uint8_t* p = b->data + b->pixels + (uint32_t)row * b->stride;
    if (b->bpp == 8) {
        // a palette number: point at its colour in the palette instead
        uint32_t index = p[x];
        if (index >= b->colours) {
            return 0; // past the end of the palette (a damaged file): black rather than garbage
        }
        p = b->palette + index * 4;
    } else {
        p += (uint32_t)x * (b->bpp / 8); // 3 or 4 bytes a pixel (a 32 bit pixel's 4th is alpha)
    }
    // pixels and palette entries alike are blue, green, red: turn them round into 0xRRGGBB
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

// the size of a BMP picture, false if it isn't one we can show
bool gfx_image_size(const uint8_t* data, uint32_t length, int* w, int* h) {
    bmp_t b;
    if (!bmp_open(data, length, &b)) {
        return false;
    }
    *w = b.width;
    *h = b.height;
    return true;
}

// draw a BMP picture at x, y, scaled to fit in max_w x max_h when those are above 0
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
        // compare the shapes without dividing: width / height <= max_w / max_h is the same as
        // width * max_h <= height * max_w. If the picture is the narrower shape, its height
        // fills the box and the width follows; otherwise the width fills it
        if ((uint32_t)b.width * (uint32_t)max_h <= (uint32_t)b.height * (uint32_t)max_w) {
            h = (uint32_t)max_h;
            w = (uint32_t)b.width * (uint32_t)max_h / (uint32_t)b.height;
        } else {
            w = (uint32_t)max_w;
            h = (uint32_t)b.height * (uint32_t)max_w / (uint32_t)b.width;
        }
        // a very long thin picture could round down to 0 one way, and then draw nothing
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
    // starts past the right or bottom edge: none of it shows, but it is still a good picture
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
    // (pixel tx of w comes from pixel tx * width / w of the picture, the same fraction across)
    for (uint32_t ty = y0; ty < y1; ty++) {
        int py = (int)(ty * (uint32_t)b.height / h);
        for (uint32_t tx = x0; tx < x1; tx++) {
            int px = (int)(tx * (uint32_t)b.width / w);
            put((uint32_t)(x + (int)tx), (uint32_t)(y + (int)ty), pack(bmp_pixel(&b, px, py)));
        }
    }
    return true;
}

// ---- overlays (see gfx_overlay_begin in gfx.h) --------------------------------------------------

struct gfx_overlay {
    int x, y, w, h;          // the area kept, already cut to fit on the screen
    uint8_t* old_target;     // where drawing went before (the screen, or the back buffer)
    uint32_t old_pitch;
    uint8_t pixels[];        // the area's pixels as they were, w * fb_bytes bytes a row
};

// keep what the screen shows in an area, and draw straight onto the screen until gfx_overlay_end
gfx_overlay_t* gfx_overlay_begin(int x, int y, int w, int h) {
    if (!active) {
        return NULL;
    }
    // only the part that is on the screen
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb.width) { w = (int)fb.width - x; }
    if (y + h > (int)fb.height) { h = (int)fb.height - y; }
    if (w <= 0 || h <= 0) {
        return NULL;
    }

    // one allocation holds both the struct and the pixels kept, which go in its pixels[] at the end
    uint32_t row_bytes = (uint32_t)w * fb_bytes;
    gfx_overlay_t* overlay = malloc(sizeof(gfx_overlay_t) + row_bytes * (uint32_t)h);
    if (overlay == NULL) {
        return NULL;
    }
    overlay->x = x;
    overlay->y = y;
    overlay->w = w;
    overlay->h = h;

    // keep what the screen shows there now (the screen itself, not the back buffer: it is the
    // screen that has to look the same afterwards), a row at a time as the rows are pitch apart
    uint8_t* screen = (uint8_t*)(uintptr_t)fb.address;
    for (int row = 0; row < h; row++) {
        memcpy(overlay->pixels + (uint32_t)row * row_bytes,
               screen + (uint32_t)(y + row) * fb.pitch + (uint32_t)x * fb_bytes, row_bytes);
    }

    // and draw straight onto the screen until the overlay ends
    overlay->old_target = target;
    overlay->old_pitch = target_pitch;
    target = screen;
    target_pitch = fb.pitch;
    return overlay;
}

// put back what was under the overlay, and send drawing wherever it went before
void gfx_overlay_end(gfx_overlay_t* overlay) {
    if (overlay == NULL) {
        return;
    }
    uint8_t* screen = (uint8_t*)(uintptr_t)fb.address;
    uint32_t row_bytes = (uint32_t)overlay->w * fb_bytes;
    // the kept pixels back onto the screen, row by row
    for (int row = 0; row < overlay->h; row++) {
        memcpy(screen + (uint32_t)(overlay->y + row) * fb.pitch + (uint32_t)overlay->x * fb_bytes,
               overlay->pixels + (uint32_t)row * row_bytes, row_bytes);
    }
    target = overlay->old_target;
    target_pitch = overlay->old_pitch;
    free(overlay);
}

/*
    splash.c - the AerOS boot screen

    Text mode only gives us 80 x 25 character cells and 16 colours, so the artwork is built
    from a few tricks:
    - Colours: none of the 16 standard colours is orange, but each of them is just an entry in
      the VGA palette, so while the splash is up we repaint brown, light red and yellow into
      the AerOS orange and two lighter shades of it (splash_close puts them back).
    - The wordmark: a text cell has a foreground and a background colour, and the character
      0xDF (an upper half block) paints the top half of the cell in one and the bottom half in
      the other. That turns the screen into an 80 x 50 grid of chunky pixels, each with its own
      colour. The "aerOS" bitmap below was made by rendering the word in Arial and measuring how
      much of each pixel the letters cover; partly covered pixels get one of the lighter oranges,
      which smooths the edges.
    - The box and the line use the font's box drawing characters, and the copyright sign is a
      glyph vga_init adds to the font.
*/

#include "splash.h"
#include <stdint.h>
#include "../drivers/video/VGA_linear.h"

// the borrowed colours
#define SPLASH_ORANGE BROWN     // the background, #FF7F00
#define SPLASH_LIGHT  LIGHT_RED // a third of the way from orange to white
#define SPLASH_PALE   YELLOW    // two thirds of the way

#define LOGO_COLS 54
#define LOGO_ROWS 16            // pixel rows, two per text row

// "aerOS", one digit per pixel: 0 = orange, 1 and 2 = the in between shades, 3 = white
static const char* const logo[LOGO_ROWS] = {
    "000000000000000000000000000000012333210000000123332100",
    "000000000000000000000000000000133333332000001333333310",
    "000000000000000000000000000001331000033200003310001330",
    "000000000000000000000000000003310000003310013100000231",
    "001222200000012221000011122013200000001320013100000121",
    "023333330000233333100032333123100000000330013300000000",
    "132000232001320003310033211033000000000231002332110000",
    "230000032003300001320033000033000000000231000233333200",
    "000000132003200000330033000033000000000231000001233330",
    "001233332013333333330032000033000000000231000000001332",
    "133332132013322222220032000023100000000330011000000033",
    "231000032013200000000032000013200000001320033000000033",
    "330000132003300000320032000003310000003310023100000033",
    "330000332002320002320032000001331000033200013310001232",
    "233223333000333233300032000000133333332000001333333320",
    "023332022000023332000022000000002333210000000123333100",
};

static const enum color shades[4] = { SPLASH_ORANGE, SPLASH_LIGHT, SPLASH_PALE, WHITE };

// the real colours of the borrowed palette entries, kept for splash_close
static const enum color borrowed[3] = { SPLASH_ORANGE, SPLASH_LIGHT, SPLASH_PALE };
static uint8_t saved[3][3];

// draw the wordmark with its top left corner at column x, row y
static void draw_logo(uint8_t x, uint8_t y) {
    for (int row = 0; row < LOGO_ROWS / 2; row++) {
        for (int col = 0; col < LOGO_COLS; col++) {
            enum color top = shades[logo[row * 2][col] - '0'];
            enum color bottom = shades[logo[row * 2 + 1][col] - '0'];
            if (top == bottom) {
                putc(x + col, y + row, top, top, ' ');
            } else {
                putc(x + col, y + row, top, bottom, (char)0xDF);
            }
        }
    }
}

// a single line box around text, with its top left corner at column x, row y
static void draw_box(uint8_t x, uint8_t y, const char* text) {
    uint8_t width = 0;
    while (text[width]) {
        width++;
    }
    putc(x, y, WHITE, SPLASH_ORANGE, (char)0xDA);              // top left corner
    putc(x, y + 1, WHITE, SPLASH_ORANGE, (char)0xB3);          // left side
    putc(x, y + 2, WHITE, SPLASH_ORANGE, (char)0xC0);          // bottom left corner
    for (uint8_t i = 1; i <= width; i++) {
        putc(x + i, y, WHITE, SPLASH_ORANGE, (char)0xC4);      // top edge
        putc(x + i, y + 2, WHITE, SPLASH_ORANGE, (char)0xC4);  // bottom edge
    }
    putc(x + width + 1, y, WHITE, SPLASH_ORANGE, (char)0xBF);     // top right corner
    putc(x + width + 1, y + 1, WHITE, SPLASH_ORANGE, (char)0xB3); // right side
    putc(x + width + 1, y + 2, WHITE, SPLASH_ORANGE, (char)0xD9); // bottom right corner
    puts(x + 1, y + 1, WHITE, SPLASH_ORANGE, text);
}

void splash_draw(void) {
    for (int i = 0; i < 3; i++) {
        vga_get_palette(borrowed[i], &saved[i][0], &saved[i][1], &saved[i][2]);
    }
    vga_set_palette(SPLASH_ORANGE, 255, 127, 0);
    vga_set_palette(SPLASH_LIGHT, 255, 170, 85);
    vga_set_palette(SPLASH_PALE, 255, 212, 170);

    clear(SPLASH_ORANGE);

    // top: the copyright box on the left, the version on the right
    draw_box(2, 1, " Alexander Walford 2026 " VGA_COPYRIGHT " ");
    puts(74, 2, WHITE, SPLASH_ORANGE, "V0.1");

    // bottom right: the hint, a line, and the wordmark underneath, all ending at column 77
    puts(53, 15, WHITE, SPLASH_ORANGE, "Press X for boot options.");
    for (uint8_t x = 22; x <= 77; x++) {
        putc(x, 16, WHITE, SPLASH_ORANGE, (char)0xC4);
    }
    draw_logo(78 - LOGO_COLS, 17);
}

void splash_close(void) {
    // blank the screen first, or the splash would flash brown as the colours change back
    clear(BLACK);
    for (int i = 0; i < 3; i++) {
        vga_set_palette(borrowed[i], saved[i][0], saved[i][1], saved[i][2]);
    }
}

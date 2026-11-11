/*
    ui.c - progress bars and spinners for the text screen (see ui.h)
*/

#include "ui.h"
#include "../lib/primary_definitions.h"
#include "../lib/busy.h"
#include "sys_clck.h"

#define FRAME_MS    100 // redraw at most this often (10 a second is smooth enough for text)
#define BAR_FULL    ((char)0xDB) // █
#define BAR_EMPTY   ((char)0xB0) // ░
#define LABEL_X     44  // where a progress bar's label starts (2 + 1 + 30 + 1 + 6 + 4 spaces)
#define BAR_WIDTH   30

// ---- spinners -----------------------------------------------------------------------------------
// each style is a list of frames of the same width, shown for ms milliseconds each

static const char* const line_frames[] = { "|", "/", "-", "\\" };
static const char* const arrow_frames[] = { "\x18", "\x1A", "\x19", "\x1B" };          // ↑ → ↓ ←
static const char* const pulse_frames[] = { "\xB0", "\xB1", "\xB2", "\xDB", "\xB2", "\xB1" }; // ░▒▓█▓▒
static const char* const dot_frames[] = { "\xF9  ", " \xF9 ", "  \xF9", " \xF9 " };     // ∙ moving
static const char* const bounce_frames[] = {
    "[\xFE    ]", "[ \xFE   ]", "[  \xFE  ]", "[   \xFE ]", "[    \xFE]",                // ■ bouncing
    "[   \xFE ]", "[  \xFE  ]", "[ \xFE   ]",
};

static const struct {
    const char* const* frames;
    int count;
    uint32_t ms;
} styles[UI_SPINNER_STYLES] = {
    { line_frames, 4, 100 },
    { arrow_frames, 4, 120 },
    { pulse_frames, 6, 100 },
    { dot_frames, 4, 150 },
    { bounce_frames, 8, 90 },
};

// draw the current frame of a spinner at column x, row y, returns how many columns it took
int ui_spinner (uint8_t x, uint8_t y, int style, enum color colour) {
    // an unknown style gets the plain | / - \ one rather than reading past the table
    if (style < 0 || style >= UI_SPINNER_STYLES) {
        style = 0;
    }
    // the frame follows the clock, so the speed doesn't depend on how often this is called
    // (milliseconds / ms per frame = how many frames have gone by, % count wraps it round)
    int frame = (int)(clkuptms() / styles[style].ms % (uint32_t)styles[style].count);
    const char* s = styles[style].frames[frame];
    int width = 0;
    // a character at a time, stopping at the right edge of the screen
    for (; *s && x + width < COLS; s++, width++) {
        putc((uint8_t)(x + width), y, colour, console_bg, *s);
    }
    return width;
}

// ---- progress bars ------------------------------------------------------------------------------

// draw a progress bar "[█████░░░░░]  50%" at column x, row y, `width` cells between the brackets
void ui_bar (uint8_t x, uint8_t y, uint8_t width, uint32_t done, uint32_t total, enum color colour) {
    // a caller that counts past the total just gets a full bar, not one over 100%
    if (done > total) {
        done = total;
    }
    // How much is done, as a percentage. done * 100 would overflow 32 bits once done passes
    // about 42 million, so for big totals divide the total down first instead (a little less
    // exact, but only by a fraction of a percent)
    uint32_t percent;
    if (total == 0) {
        percent = 100;                       // nothing to do counts as all done
    } else if (total < 40000000) {
        percent = done * 100 / total;
    } else {
        percent = done / (total / 100);
    }
    uint32_t filled = percent * width / 100; // how many cells of the bar are full

    // the bar itself, cut off at the right edge of the screen
    putc(x, y, BRIGHT, console_bg, '[');
    for (uint32_t i = 0; i < width && x + 1 + i < COLS; i++) {
        uint8_t cell_x = (uint8_t)(x + 1 + i);
        if (i < filled) {
            putc(cell_x, y, colour, console_bg, BAR_FULL);
        } else {
            putc(cell_x, y, DARK_GRAY, console_bg, BAR_EMPTY);
        }
    }
    // then the closing bracket and the percentage, padded to 3 digits so the bar's end doesn't
    // jump about as the number grows ("%%" is a plain %)
    char text[8];
    sfmt(text, sizeof(text), "] %3u%%", percent);
    for (int i = 0; text[i] && x + 1 + width + i < COLS; i++) {
        putc((uint8_t)(x + 1 + width + i), y, BRIGHT, console_bg, text[i]);
    }
}

// blank a row from column x to the end
static void clear_row (uint8_t x, uint8_t y) {
    for (; x < COLS; x++) {
        putc(x, y, console_fg, console_bg, ' ');
    }
}

// start a progress bar on a line of its own at the cursor, for `total` things to do
void ui_progress_begin (ui_progress_t* p, uint32_t total) {
    uint8_t x, y;
    console_get_cursor(&x, &y);
    // part way along a line: start a new one, so the bar doesn't draw over what is there
    if (x != 0) {
        prtf("\n");
        console_get_cursor(&x, &y);
    }
    // print the line first (a newline at the bottom scrolls the screen), then draw over it
    prtf("\n");
    console_get_cursor(&x, &y);
    p->y = (uint8_t)(y - 1); // the line just printed, now that the scrolling has happened
    p->total = total;
    p->last_ms = 0;          // 0 = never drawn, so the first update always draws
    ui_bar(2, p->y, BAR_WIDTH, 0, total, LIGHT_GREEN);
}

// move the bar on to `done`, with a label after it (NULL for none)
void ui_progress_update (ui_progress_t* p, uint32_t done, const char* label) {
    uint32_t now = clkuptms();
    // Drawing is slow next to the work being counted, so only redraw every FRAME_MS. The
    // first update and the last (done reaching the total) are always drawn, so the bar
    // never stops short of 100%
    if (p->last_ms != 0 && now - p->last_ms < FRAME_MS && done < p->total) {
        return;
    }
    // 0 means "never drawn", so if the clock happens to read 0 (just after boot, or the timer
    // not running) 1 is kept instead
    p->last_ms = now ? now : 1;
    ui_bar(2, p->y, BAR_WIDTH, done, p->total, LIGHT_GREEN);
    // blank the old label first, a shorter new one would leave the end of it behind
    clear_row(LABEL_X, p->y);
    if (label) {
        // in grey after the bar, cut off a column short of the right edge if it is too long
        for (int i = 0; label[i] && LABEL_X + i < COLS - 1; i++) {
            putc((uint8_t)(LABEL_X + i), p->y, DARK_GRAY, console_bg, label[i]);
        }
    }
}

// finish a progress bar: drawn full, with the label taken away
void ui_progress_end (ui_progress_t* p) {
    ui_bar(2, p->y, BAR_WIDTH, p->total, p->total, LIGHT_GREEN);
    clear_row(LABEL_X, p->y);
    // the cursor is already on the line below the bar (ui_progress_begin printed it)
}

// ---- busy spinner -------------------------------------------------------------------------------

static uint8_t busy_y;        // the row the spinner is on
static uint32_t busy_last_ms; // when it was last drawn

// what busy() calls while a spinner is up
static void busy_turn (void) {
    // busy() can be called thousands of times a second, so most calls do nothing at all
    uint32_t now = clkuptms();
    if (now - busy_last_ms < FRAME_MS) {
        return;
    }
    busy_last_ms = now;
    ui_spinner(2, busy_y, 0, YELLOW);
}

// show a spinner and a label on a line of their own, turning whenever the work calls busy()
void ui_busy_begin (const char* label) {
    uint8_t x, y;
    console_get_cursor(&x, &y);
    if (x != 0) {
        prtf("\n");
    }
    // "  |  Checking the password" (the spinner goes in column 2)
    // the 5 spaces leave room for it, and printing the label first means any scrolling has
    // happened before we note which row it is on
    prtf("     %s", label);
    console_get_cursor(&x, &y);
    busy_y = y;
    busy_last_ms = 0;
    ui_spinner(2, busy_y, 0, YELLOW);
    // from now on every busy() call turns the spinner (see lib/busy.h)
    busy_hook = busy_turn;
}

// take the spinner and its label away, leaving the cursor at the start of their now blank line
void ui_busy_end (void) {
    busy_hook = 0; // first, so nothing redraws the spinner while it is being cleared
    clear_row(0, busy_y);
    console_set_cursor(0, busy_y);
}

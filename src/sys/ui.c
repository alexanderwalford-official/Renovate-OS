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

int ui_spinner (uint8_t x, uint8_t y, int style, enum color colour) {
    if (style < 0 || style >= UI_SPINNER_STYLES) {
        style = 0;
    }
    // the frame follows the clock, so the speed doesn't depend on how often this is called
    int frame = (int)(clck_uptime_ms() / styles[style].ms % (uint32_t)styles[style].count);
    const char* s = styles[style].frames[frame];
    int width = 0;
    for (; *s && x + width < COLS; s++, width++) {
        putc((uint8_t)(x + width), y, colour, console_bg, *s);
    }
    return width;
}

// ---- progress bars ------------------------------------------------------------------------------

void ui_bar (uint8_t x, uint8_t y, uint8_t width, uint32_t done, uint32_t total, enum color colour) {
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

    putc(x, y, BRIGHT, console_bg, '[');
    for (uint32_t i = 0; i < width && x + 1 + i < COLS; i++) {
        uint8_t cell_x = (uint8_t)(x + 1 + i);
        if (i < filled) {
            putc(cell_x, y, colour, console_bg, BAR_FULL);
        } else {
            putc(cell_x, y, DARK_GRAY, console_bg, BAR_EMPTY);
        }
    }
    char text[8];
    snprintf(text, sizeof(text), "] %3u%%", percent);
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

void ui_progress_begin (ui_progress_t* p, uint32_t total) {
    uint8_t x, y;
    console_get_cursor(&x, &y);
    if (x != 0) {
        printf("\n");
        console_get_cursor(&x, &y);
    }
    // print the line first (a newline at the bottom scrolls the screen), then draw over it
    printf("\n");
    console_get_cursor(&x, &y);
    p->y = (uint8_t)(y - 1);
    p->total = total;
    p->last_ms = 0;
    ui_bar(2, p->y, BAR_WIDTH, 0, total, LIGHT_GREEN);
}

void ui_progress_update (ui_progress_t* p, uint32_t done, const char* label) {
    uint32_t now = clck_uptime_ms();
    if (p->last_ms != 0 && now - p->last_ms < FRAME_MS && done < p->total) {
        return;
    }
    p->last_ms = now ? now : 1;
    ui_bar(2, p->y, BAR_WIDTH, done, p->total, LIGHT_GREEN);
    clear_row(LABEL_X, p->y);
    if (label) {
        for (int i = 0; label[i] && LABEL_X + i < COLS - 1; i++) {
            putc((uint8_t)(LABEL_X + i), p->y, DARK_GRAY, console_bg, label[i]);
        }
    }
}

void ui_progress_end (ui_progress_t* p) {
    ui_bar(2, p->y, BAR_WIDTH, p->total, p->total, LIGHT_GREEN);
    clear_row(LABEL_X, p->y);
    // the cursor is already on the line below the bar (ui_progress_begin printed it)
}

// ---- busy spinner -------------------------------------------------------------------------------

static uint8_t busy_y;
static uint32_t busy_last_ms;

// what busy() calls while a spinner is up
static void busy_turn (void) {
    uint32_t now = clck_uptime_ms();
    if (now - busy_last_ms < FRAME_MS) {
        return;
    }
    busy_last_ms = now;
    ui_spinner(2, busy_y, 0, YELLOW);
}

void ui_busy_begin (const char* label) {
    uint8_t x, y;
    console_get_cursor(&x, &y);
    if (x != 0) {
        printf("\n");
    }
    // "  |  Checking the password" (the spinner goes in column 2)
    printf("     %s", label);
    console_get_cursor(&x, &y);
    busy_y = y;
    busy_last_ms = 0;
    ui_spinner(2, busy_y, 0, YELLOW);
    busy_hook = busy_turn;
}

void ui_busy_end (void) {
    busy_hook = 0;
    clear_row(0, busy_y);
    console_set_cursor(0, busy_y);
}

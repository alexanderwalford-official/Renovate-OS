/*
    ui.h
    Small animations for the text screen: progress bars and spinners.

    Progress bar, for work whose size is known (deleting 120 files):

        [████████████░░░░░░░░░░░░░░░░░░]  40%  notes.txt

    Spinner, for work whose length isn't known (checking a password):

        | / - \        line           ░ ▒ ▓ █ ▓ ▒     pulse
        ↑ → ↓ ←        arrows         [ ■    ]        bounce
        ∙              dots (a dot running along three cells)

    Both are drawn straight into character cells with putc, so they never move the cursor or
    scroll the screen, and work the same in text mode and on a framebuffer. The frame a
    spinner shows depends on the time, not on how often it is drawn, so it turns at the same
    speed however busy the caller is.

    Updates are cooperative (see lib/busy.h): the work calls ui_progress_update() or busy() as it
    goes, and the screen is only redrawn about 10 times a second, so calling them often is cheap.
    AerLang programs get the same drawing through progress(), spinner() and frame().
*/
#ifndef UI_H
#define UI_H

#include <stdint.h>
#include <stdbool.h>
#include "../drivers/video/VGA_linear.h"

#define UI_SPINNER_STYLES 5 // 0 line, 1 arrows, 2 pulse, 3 dots, 4 bounce

// draw a bar of width cells (plus brackets and the percentage after it, 7 more cells) at x, y
void ui_bar (uint8_t x, uint8_t y, uint8_t width, uint32_t done, uint32_t total, enum color colour);

// draw the current frame of a spinner at x, y, returns how many cells it takes
int ui_spinner (uint8_t x, uint8_t y, int style, enum color colour);

// a progress bar on its own line of the terminal
typedef struct {
    uint8_t y;          // the screen row it is on
    uint32_t total;     // how much work there is
    uint32_t last_ms;   // when it was last drawn, to keep to about 10 redraws a second
} ui_progress_t;

// start a bar on the current line (moving to a fresh line first if needed)
void ui_progress_begin (ui_progress_t* p, uint32_t total);
// done of total finished, label says what is happening (may be NULL); redraws at most 10x a second
void ui_progress_update (ui_progress_t* p, uint32_t done, const char* label);
// draw it full and move the terminal to the next line
void ui_progress_end (ui_progress_t* p);

// show a spinner and label on the current line, turned by busy() calls from the work
void ui_busy_begin (const char* label);
// stop: the spinner line is cleared and the cursor left at its start, so the next message replaces it
void ui_busy_end (void);

#endif /* UI_H */

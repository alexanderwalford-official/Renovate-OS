/*
    bi_screen.c
    AerLang's built-in functions for the text screen and the keyboard: clearing the screen (cls),
    colours (color), moving the cursor (at), reading keys (key, haskey), and the same animation
    the OS itself draws with (sys/ui.h): big letters, progress bars, spinners, and frame() to
    keep a loop at a steady speed.

    The graphics screen is in bi_graphics.c, and so are keydown() and the mouse, which are for
    games.

    How a built-in works, and how to add one, is at the top of builtins.h.
*/

#include "builtins.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/input/IO.h"
#include "../sys/sys_clck.h"
#include "../sys/ui.h"

// the cursor, colours and keys

// cls(): clear the screen
value_t bi_cls (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    console_clear();
    return aer_nil();
}

// color(fg, bg): the colours text is written in from now on (0 to 15, BLACK to WHITE). bg stays as it is if left out
value_t bi_color (int argc, value_t* argv, int line) {
    int32_t fg = int_arg(argv, 0, line, "color");
    int32_t bg = argc > 1 ? int_arg(argv, 1, line, "color") : (int32_t)console_bg;
    // & 15 keeps the lowest 4 bits, so any number becomes one of the 16 colours instead of an error
    colour((enum color)(fg & 15), (enum color)(bg & 15));
    return aer_nil();
}

// at(x, y): move the cursor, so the next thing written starts at column x, row y
value_t bi_at (int argc, value_t* argv, int line) {
    (void)argc;
    console_set_cursor((uint8_t)int_arg(argv, 0, line, "at"), (uint8_t)int_arg(argv, 1, line, "at"));
    return aer_nil();
}

// key(): wait for a key and give back its number: a character's code, or KEY_UP, KEY_ENTER...
value_t bi_key (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    return aer_int(getk());
}

// haskey(): is there a key waiting that key() hasn't taken yet? (never waits)
value_t bi_haskey (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    return aer_bool(kb_haschar());
}

// animation (the same drawing the OS uses, see sys/ui.h)

// a screen position from two arguments, checked so nothing is drawn off the screen
static void cell_args (value_t* argv, int line, const char* fn, uint8_t* x, uint8_t* y) {
    int32_t cx = int_arg(argv, 0, line, fn);
    int32_t cy = int_arg(argv, 1, line, fn);
    // checked as full numbers before the uint8_t casts below, which would quietly wrap 300 round to 44
    if (cx < 0 || cx >= COLS || cy < 0 || cy >= ROWS) {
        aer_error(line, "%s() position %d, %d is off the screen (it is %d x %d)", fn, cx, cy, COLS, ROWS);
    }
    *x = (uint8_t)cx;
    *y = (uint8_t)cy;
}

// big(x, y, text, colour, background): text in big letters, like the splash screen's
// "aerOS", with its top left corner at column x, row y. Each letter is 8 columns wide and 7 rows
// tall. The colour is WHITE and the background BLACK if they are left out.
// Returns how many columns wide it is, so it can be centred
value_t bi_big_text (int argc, value_t* argv, int line) {
    uint8_t x, y;
    cell_args(argv, line, "big", &x, &y);
    aer_object_t* text = str_arg(argv, 2, line, "big");

    enum color colour = WHITE;
    if (argc > 3) {
        colour = (enum color)(int_arg(argv, 3, line, "big") & 15); // one of the 16 colours
    }
    enum color background = BLACK;
    if (argc > 4) {
        background = (enum color)(int_arg(argv, 4, line, "big") & 15);
    }
    return aer_int(big_text(x, y, colour, background, text->data.chars));
}

// progress(x, y, width, done, total, colour): [#####.....]  50%
value_t bi_progress (int argc, value_t* argv, int line) {
    uint8_t x, y;
    cell_args(argv, line, "progress", &x, &y);
    int32_t width = int_arg(argv, 2, line, "progress");
    int32_t done = int_arg(argv, 3, line, "progress");
    int32_t total = int_arg(argv, 4, line, "progress");
    int32_t colour = argc > 5 ? int_arg(argv, 5, line, "progress") : LIGHT_GREEN;
    // the bar must fit on one row of the screen
    if (width < 1 || width > COLS) {
        aer_error(line, "progress() width must be 1 to %d", COLS);
    }
    // a negative amount counts as none
    if (done < 0) {
        done = 0;
    }
    if (total < 0) {
        total = 0;
    }
    enum color bar_colour = (enum color)(colour & 15); // one of the 16 text colours
    ui_bar(x, y, (uint8_t)width, (uint32_t)done, (uint32_t)total, bar_colour);
    return aer_nil();
}

// spinner(x, y, style, colour): the frame for this moment, returns how many cells it took
// (the frame comes from the clock, so calling it in a loop makes it spin). style 0 and YELLOW
// if left out
value_t bi_spinner (int argc, value_t* argv, int line) {
    uint8_t x, y;
    cell_args(argv, line, "spinner", &x, &y);
    int32_t style = argc > 2 ? int_arg(argv, 2, line, "spinner") : 0;
    int32_t colour = argc > 3 ? int_arg(argv, 3, line, "spinner") : YELLOW;
    if (style < 0 || style >= UI_SPINNER_STYLES) {
        aer_error(line, "spinner() style must be 0 to %d", UI_SPINNER_STYLES - 1);
    }
    return aer_int(ui_spinner(x, y, style, (enum color)(colour & 15)));
}

// when the last frame was due: frame() counts the time to the next one from here
static uint32_t last_frame_ms = 0;

// frame(fps): wait until it's time for the next frame, so a loop runs at a steady speed
// whatever it draws (sleep would add the drawing time on top). Ctrl+C stops the program
value_t bi_frame (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t fps = int_arg(argv, 0, line, "frame");
    if (fps < 1 || fps > 1000) {
        aer_error(line, "frame() needs 1 to 1000 frames a second");
    }
    // how long one frame lasts, in whole milliseconds (60 fps gives 16, so a touch over 60 a second)
    uint32_t period = 1000u / (uint32_t)fps;
    // a long gap (the first frame, or after something slow) starts the timing again
    // (otherwise the loop would race through several frames with no wait, to catch up)
    if (clkuptms() - last_frame_ms > period * 4) {
        last_frame_ms = clkuptms();
    }
    // wait out the rest of this frame a millisecond at a time, watching for Ctrl+C
    // (unsigned subtraction still works if the millisecond counter wraps round)
    while (clkuptms() - last_frame_ms < period) {
        if (kb_take_break()) {
            aer_stop(line);
        }
        sleep_ms(1);
    }
    // move on by exactly one period rather than to "now", so a frame that ends a little late
    // is made up by a shorter wait next time and the speed doesn't slowly drift
    last_frame_ms += period;
    return aer_nil();
}

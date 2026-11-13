/*
    trace.c - AerOS taking notes on what it does, for SHOW (see trace.h)

    The notes go into a plain array, filled from the start: SHOW wants the first steps of a
    command (how it began), so once the array is full the rest are only counted. Each note is
    formatted straight away into its slot, so a note can mention a name or a number that is
    gone by the time SHOW prints it. The same note twice in a row (a file read three times while
    a version is kept, say) becomes one step with a count, so the list stays readable.
*/

#include "trace.h"
#include <stdarg.h>
#include "../lib/aer_stdio.h"
#include "../lib/aer_string.h"
#include "../lib/primary_definitions.h"
#include "../drivers/video/VGA_linear.h"

// the screen is 80 columns: notes are wrapped to fit, under their own column
#define TRACE_SCREEN_WIDTH 80
// the function names' column is never wider than this, so a long name can't push the notes away
#define TRACE_NAME_WIDTH 20

bool trace_watching = false;

static trace_step_t steps[TRACE_MAX];
static int step_count = 0; // the steps listed so far, including ones past the end of the array

// take a note (TRACE() calls this only while something is watching)
void trace_note (const char* function, const char* format, ...) {
    char text[TRACE_TEXT];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    // the same note from the same function straight after: count it instead of listing it again
    if (step_count > 0 && step_count <= TRACE_MAX) {
        trace_step_t* last = &steps[step_count - 1];
        if (last->function == function && seq(last->text, text)) {
            last->times++;
            return;
        }
    }
    // past the end of the array the note is only counted, for "and N more"
    if (step_count < TRACE_MAX) {
        trace_step_t* step = &steps[step_count];
        step->function = function;
        scpy(step->text, text, sizeof(step->text));
        step->times = 1;
    }
    step_count++;
}

// start watching, with no steps yet
void trace_begin (void) {
    step_count = 0;
    trace_watching = true;
}

// stop watching, giving how many steps there were
int trace_end (void) {
    trace_watching = false;
    return step_count;
}

// the steps the last watch recorded, and how many of them there are
const trace_step_t* trace_steps (int* count) {
    *count = step_count < TRACE_MAX ? step_count : TRACE_MAX;
    return steps;
}

// print text from column `column` on, wrapped at word boundaries so no word is cut in two at
// the screen's edge: each line after the first starts at `column` too
static void print_wrapped (const char* text, int column) {
    int room = TRACE_SCREEN_WIDTH - column - 1; // -1: a line that fills the screen exactly
    int used = 0;                               // would leave the cursor on the next line
    while (*text != '\0') {
        // the next word (up to a space), and whether it still fits on this line
        int length = (int)strcspn(text, " ");
        if (used > 0 && used + 1 + length > room) {
            prtf("\n%*s", column, "");
            used = 0;
        } else if (used > 0) {
            prtf(" ");
            used++;
        }
        prtf("%.*s", length, text);
        used += length;
        text += length;
        while (*text == ' ') {
            text++;
        }
    }
    prtf("\n");
}

// print the steps of the last watch, numbered, with each function's name in a column of its own
// so the notes line up (the name is what WHY and SRC take)
void trace_print (void) {
    int shown;
    trace_steps(&shown);
    if (shown == 0) {
        prtf("Nothing that SHOW can see happened. Not every part of AerOS takes notes yet:\n"
             "a TRACE() line in its code is all it takes (see sys/trace.h).\n");
        return;
    }

    // the widest name sets the column
    int width = 0;
    for (int i = 0; i < shown; i++) {
        int length = (int)slen(steps[i].function);
        if (length > width) {
            width = length;
        }
    }
    if (width > TRACE_NAME_WIDTH) {
        width = TRACE_NAME_WIDTH;
    }

    colour(LIGHT_CYAN, BLACK);
    prtf("What AerOS did:\n");
    colour(BRIGHT, BLACK);
    for (int i = 0; i < shown; i++) {
        // "  3. fat_free_chain   " then the note, which starts after the name's column
        prtf("%3d. ", i + 1);
        colour(YELLOW, BLACK);
        prtf("%-*s  ", width, steps[i].function);
        colour(BRIGHT, BLACK);
        char text[TRACE_TEXT + 16];
        if (steps[i].times > 1) {
            sfmt(text, sizeof(text), "%s (%d times)", steps[i].text, steps[i].times);
        } else {
            scpy(text, steps[i].text, sizeof(text));
        }
        print_wrapped(text, 5 + width + 2);
    }
    if (step_count > shown) {
        prtf("     ... and %d more step(s), left out to keep this readable\n", step_count - shown);
    }
    colour(DARK_GRAY, BLACK);
    prtf("WHY <name> explains a step, SRC <name> shows its code.\n");
    colour(BRIGHT, BLACK);
}

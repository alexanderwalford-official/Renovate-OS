/*
    trace.h - AerOS taking notes on what it does, for SHOW

    WHY explains what went wrong after the fact, and SRC shows the code. SHOW goes one further:
    it runs a command and then tells you, step by step, what AerOS did inside to carry it out:

        show del notes.txt

        What AerOS did:
          1. fs_delete_f        notes.txt in /home/alex/ is on the disk (FAT)
          2. versions_keep      kept it as notes.txt@1 first, so UNDO can bring it back
          3. fat_delete_file    marked its entry deleted, then freed its 1 cluster
        WHY <name> explains a step, SRC <name> shows its code.

    Each step is one TRACE() line in the code, saying in plain words what that function decided
    or did (not every little thing: what someone reading the code would want to know):

        TRACE("kept it as %s@1 first, so UNDO can bring it back", name);

    TRACE() records the function it is in by itself (C's __func__), so WHY and SRC can be pointed
    straight at it. While nothing is watching, a TRACE() costs one check of a flag and nothing
    else, so it is fine anywhere, even in code that runs often. (It mustn't be used in an
    interrupt handler, as the notes are kept without locking.)

    Adding to it: put a TRACE() where a function makes a decision or does something worth
    knowing. That is all: SHOW, and AerLang's show(), list whatever was recorded, in order.
*/

#ifndef TRACE_H
#define TRACE_H

#include <stdbool.h>

// the most steps one SHOW keeps. Any more are counted, and SHOW says how many it left out
#define TRACE_MAX 48
// the longest note, including its '\0' (a longer one is cut short)
#define TRACE_TEXT 100

// one step: the function that took the note, what it said, and how many times in a row (the
// same note again straight after is counted rather than listed twice)
typedef struct {
    const char* function;
    char text[TRACE_TEXT];
    int times;
} trace_step_t;

// is anything watching right now? (TRACE() checks this first, so it costs nothing otherwise)
extern bool trace_watching;

// take a note: what this function did, made like prtf makes a line. Use TRACE() rather than
// calling this, so the function's own name goes with it
void trace_note (const char* function, const char* format, ...) __attribute__((format(printf, 2, 3)));

#define TRACE(...) do { if (trace_watching) { trace_note(__func__, __VA_ARGS__); } } while (0)

// start watching, with no steps yet. Watching doesn't nest: a SHOW inside a SHOW starts again
void trace_begin (void);

// stop watching. Gives how many steps there were (more than TRACE_MAX if some were left out)
int trace_end (void);

// the steps recorded by the last watch, in order, and how many there are (at most TRACE_MAX)
const trace_step_t* trace_steps (int* count);

// print the steps of the last watch, numbered, the way SHOW shows them
void trace_print (void);

#endif /* TRACE_H */

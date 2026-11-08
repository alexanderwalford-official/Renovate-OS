#ifndef REN_COMP_H
#define REN_COMP_H

// RenLang for AerOS compiler

/*
    RenLang is the tiny language every AerOS process is written in. Each process holds
    one line of it, and compile() "compiles" that line by matching it against the instructions
    it knows and calling the matching kernel function straight away (so it is really closer to
    an interpreter than a compiler). See compile() in ren_comp.c for the full instruction list,
    e.g. "splash_screen", "init_boot", "terminal", "print_stack" or "s:<text>" to print text.

    Anything it does not recognise brings up the red system exception screen.

    COLOUR GUIDE FOR PROGRAMMING INTERFACE:
    0 = BLACK
    1 = BRIGHT
    (these are the two colours the full screen layouts here are drawn in, BLACK text on a
    BRIGHT (light grey) background, the full 16 colour list is enum color in VGA_linear.h)
*/

// pass arguments to compiler (a placeholder for now, it ignores arg and always returns 0)
int comp_handler (char* arg);

// the boot logo, shown for up to 4 seconds. Sets boot_options_requested (and stops early) if X is pressed. Returns 0
int splash_screen (void);

// the red "SYSTEM EXCEPTION" screen with a beep, held for 4 seconds. code = "invalid_code" or anything else. Returns 0
int error_screen (const char* code);

// run one line of RenLang code, returns 0 on success or -1 for code it does not understand
int compile (const char* code);

#endif /* REN_COMP_H */

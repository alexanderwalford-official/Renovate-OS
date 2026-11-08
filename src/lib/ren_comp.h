#ifndef REN_COMP_H
#define REN_COMP_H

// RenLang for Renovate OS compiler

/*
    COLOUR GUIDE FOR PROGRAMMING INTERFACE:
    0 = BLACK
    1 = BRIGHT
*/

int comp_handler (char* arg);
int splash_screen (void);
int error_screen (const char* code);
int compile (const char* code);

#endif /* REN_COMP_H */

// terminal interface

#ifndef T_INT_H
#define T_INT_H

#include <stdbool.h>

extern bool cli_active; // true while the terminal is running

int flash_pointer (int times, int lc);
int cli_init (int ent);
int cli_rend_main (void);
void cli_prompt (void);
void HandleInput (char* in);

#endif /* T_INT_H */

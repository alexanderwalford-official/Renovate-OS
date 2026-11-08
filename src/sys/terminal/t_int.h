// terminal interface

/*
    t_int.h
    The AerOS terminal (command line shell). It is started by the "terminal" process
    at the end of the boot and runs until the computer is rebooted or turned off:
    show the banner, then forever: print the prompt, read a line, run the command.
    Type HELP in it for the list of commands.
*/

#ifndef T_INT_H
#define T_INT_H

#include <stdbool.h>

extern bool cli_active; // true while the terminal is running

// flash a block at the cursor (half a second on, half off), times = how many flashes, lc = how many
// are already done (so times - lc flashes happen). Blocks while it flashes, returns 0
int flash_pointer (int times, int lc);

// start the terminal and run its loop, ent = 1 shows the login screen first (in DISK mode),
// ent = 0 skips it and runs as guest
// only returns if cli_active is ever cleared, which nothing does yet
int cli_init (int ent);

// clear the screen and draw the terminal's banner (version, storage mode, HELP hint). Returns 0
int cli_rend_main (void);

// print the prompt, "0/" followed by the current directory
void cli_prompt (void);

// run one line typed by the user: the first word picks the command, the rest is its arguments
// note: in is changed in place (trimmed and split)
void HandleInput (char* in);

#endif /* T_INT_H */

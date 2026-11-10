/*
    t_int.h - the AerOS terminal (the command line)

    The terminal is started by the "terminal" process at the end of the boot, and runs until
    the computer is restarted or turned off: show the login screen and the banner, then over
    and over: print the prompt, read a line, run the command.

    The commands themselves, and how to add or rename one, are in t_int.c. Type HLP (help) in
    the terminal for the list.
*/

#ifndef T_INT_H
#define T_INT_H

#include <stdbool.h>
#include <stddef.h>

// true while the terminal is running
extern bool cli_active;

// flash a solid block at the cursor, half a second on and half a second off.
// times = how many flashes, already_done = how many have happened already (so
// times - already_done more happen). Waits while it flashes, returns 0
int flash_pointer (int times, int already_done);

// start the terminal and run it. ask_login = 1 shows the login screen first (only in DISK
// mode, when there is somewhere to keep accounts), 0 skips it and runs as "guest".
// Only returns if cli_active is ever set to false, which nothing does yet
int cli_init (int ask_login);

// clear the screen and draw the terminal's banner: the version, where files are kept, and
// how to get help. Returns 0
int cli_rend_main (void);

// print the prompt: who is logged in, "@0/", then the current folder
void cli_prompt (void);

// run one line typed by the user: the first word picks the command, the rest is its arguments.
// A line that isn't a command or an app is AerLang (see "The shell" in lang/aer_interp.c).
// Note: `in` is changed (trimmed and split up) while it runs
void HandleInput (char* in);

// run a line like HandleInput, but keep what it prints in `out` (size bytes, always ended with
// a '\0') instead of showing it. While it runs the keyboard counts as unplugged, so a question
// the command asks gets no answer. For AerLang's cmd() (lang/aer_builtins.c)
void cli_capture (const char* line, char* out, size_t size);

// the name (in capitals) of the command that runs a C function: "cli_list" -> "LA". NULL if no
// command does. Lets text elsewhere (sys/explain.c) name commands without hard-coding them, so
// renaming a command in the table renames it everywhere
const char* cli_command_called (const char* function);

#endif /* T_INT_H */

/*
    aerlang.h
    AerLang, the AerOS programming language. Programs are plain text files ending in ".aer",
    run by an interpreter inside the kernel. A quick taste:

        # comments start with a hash
        let name = input("What's your name? ")
        func greet(who)
            return "Hello " + who + "!"
        end
        for i = 1 to 3 do
            print(greet(name), i)
        end

    The full language is described in docs/AERLANG.md.
*/
#ifndef AERLANG_H
#define AERLANG_H

#include <stdbool.h>
#include <stddef.h>

// run a program file (absolute, relative to the current directory, or found in /bin/)
// args is the rest of the command line, handed to the program as the list "args"
// returns 0 if it ran to the end, -1 if it wasn't found or stopped with an error
int aer_run_file (const char* path, const char* args);

// run a program held in memory, name is only used in error messages
int aer_run_source (const char* source, const char* name, const char* args);

// the interactive AerLang prompt, lines are run as they are typed until "exit"
void aer_repl (void);

// was the last program stopped with Ctrl+C? (asking clears it)
bool aer_take_stopped (void);

// ---- the shell: AerLang at the terminal's prompt (see "The shell" in aer_interp.c) ----------

// start the shell's session (when someone logs in), or end it and forget its variables (when
// they log out)
void aer_shell_start (void);
void aer_shell_stop (void);

// Is a line typed at the prompt meant as AerLang? The terminal asks once it knows the line isn't
// a command or an app. Everything is, except what looks like a mistyped command: a plain word
// AerLang doesn't know, alone or followed by more words ("frob", "dle notes.txt")
bool aer_shell_wants (const char* line);

// run code typed at the prompt in the shell's session: the value of an expression is shown,
// and the variables and functions it makes are kept for the lines after it
void aer_shell_run (const char* code);

// how many blocks (if, while, for, func) code opens without closing them with end. More than
// 0 means more lines are coming: after "for f in la() do", the terminal waits for the "end"
int aer_open_blocks (const char* code);

// the name of the C function behind a built-in AerLang function: "gopen" -> "bi_gfx_open".
// NULL if there is no built-in called that. (WHY GOPEN and SRC GOPEN use it)
const char* aer_builtin_function (const char* name);

// does this file name (its first length characters) end in ".aer"? Programs from before the
// language was renamed end in ".ren", and count too, so they still run from an older disk
bool aer_is_program_name (const char* name, size_t length);

#endif /* AERLANG_H */

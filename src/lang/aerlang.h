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

// does this file name (its first length characters) end in ".aer"? Programs from before the
// language was renamed end in ".ren", and count too, so they still run from an older disk
bool aer_is_program_name (const char* name, size_t length);

#endif /* AERLANG_H */

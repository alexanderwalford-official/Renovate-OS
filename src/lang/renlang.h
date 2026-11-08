/*
    renlang.h
    RenLang, the AerOS programming language. Programs are plain text files ending in ".ren",
    run by an interpreter inside the kernel. A quick taste:

        # comments start with a hash
        let name = input("What's your name? ")
        func greet(who)
            return "Hello " + who + "!"
        end
        for i = 1 to 3 do
            print(greet(name), i)
        end

    The full language is described in docs/RENLANG.md.
*/
#ifndef RENLANG_H
#define RENLANG_H

// run a program file (absolute, relative to the current directory, or found in /bin/)
// args is the rest of the command line, handed to the program as the list "args"
// returns 0 if it ran to the end, -1 if it wasn't found or stopped with an error
int ren_run_file (const char* path, const char* args);

// run a program held in memory, name is only used in error messages
int ren_run_source (const char* source, const char* name, const char* args);

// the interactive RenLang prompt, lines are run as they are typed until "exit"
void ren_repl (void);

#endif /* RENLANG_H */

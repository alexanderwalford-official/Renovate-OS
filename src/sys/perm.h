/*
    perm.h - programs ask before they touch your files

    An AerLang program can read the system's own files freely, but before it reads anything
    else, or changes any file at all, AerOS asks you:

        [ ? ] viewer.aer wants to read files in /home/alex/ (cat.bmp)
              Y = yes, this time    A = always    N = no

    (in a box over the picture instead, if the program is using graphics). The answers:

        Y   allowed until the program ends
        A   allowed from now on: kept in /acc/perms. PERM lists these, PERM DEL forgets them
        N   refused until the program ends. The function fails as if the file wasn't there,
            so the program carries on (rdf gives nil, wrf gives false...)

    WHAT NEEDS ASKING

        reading in /bin/, /docs/, /src/ or /sys/     never asks: the system's own files
        reading in the program's own folder          never asks: its own files
        reading a file or folder named after the     never asks: typing "read notes.txt"
          program's name when it was started           was you asking for it
        reading anything else                        asks
        changing or deleting any file                asks
        anything in /acc/ (the accounts)             never allowed, whatever the answer

    A permission is for one folder (not the folders inside it), so a program that reads ten
    pictures from the same folder asks once. Being allowed to change files there includes
    reading them.

    The terminal's own commands, and code typed at its prompt, never ask: that is you. (So
    AerLang's cmd(), which runs a terminal command, only works at the prompt.)

    The checks are made by AerLang's file functions (lang/bi_files.c, through file_arg and
    allowed_path in lang/bi_args.c), which call
    perm_allows() before they do anything.
*/
#ifndef PERM_H
#define PERM_H

#include <stdbool.h>

// what a program wants to do with a file
typedef enum {
    PERM_READ,      // read it (or list a folder, or see whether a file is there)
    PERM_CHANGE,    // make it, change it or delete it (reading it too)
} perm_kind_t;

// a program is starting, or has ended (lang/aer_interp.c). folder and name say where the
// program's own file is ("/bin/", "viewer.aer"), args is what was typed after its name
void perm_program_started (const char* folder, const char* name, const char* args);
void perm_program_ended (void);

// May the running program read (PERM_READ) or change (PERM_CHANGE) the file `name` in
// `folder`? Asks if it needs to (see above). name is "" when it is about the folder itself
// (listing it). Always true when no program is running
bool perm_allows (const char* folder, const char* name, perm_kind_t kind);

// the "always" answers the logged in user has given (PERM lists them): fn is called for each,
// numbered from 1. Returns how many there are
typedef void (*perm_each_fn)(int number, const char* program, perm_kind_t kind, const char* folder, void* ctx);
int perm_each (perm_each_fn fn, void* ctx);

// forget one of those answers (its number from perm_each), or all of them (number 0).
// Returns how many were forgotten
int perm_forget (int number);

#endif /* PERM_H */

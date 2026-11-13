/*
    apps.h
    The apps and files that come with AerOS. Everything in src/disk/ is built into the kernel and
    copied to the same place in AerOS at boot (src/disk/bin/ into /bin/, src/disk/docs/ into
    /docs/, src/disk/sys/ into /sys/...), so it is there even without a disk.
*/
#ifndef APPS_H
#define APPS_H

#include <stdint.h>
#include <stdbool.h>

// one built-in file: where it goes in AerOS, and where its bytes are inside the kernel
typedef struct {
    const char* dir;   // the folder it is copied to, e.g. "/bin/games/"
    const char* name;  // its name, e.g. "snake.aer"
    const char* start; // the first byte of the built-in copy
    const char* end;   // one past the last byte, so end - start is its size
    bool refresh;      // true for the manual (/docs/): replaced whenever it differs. false for
                       // apps and system files, which follow the record rules in apps.c
} builtin_file_t;

// every built-in file, and how many there are. Written by the Makefile (obj/builtin_index.c) from
// what is in src/disk/, so adding a file there is all it takes
extern const builtin_file_t builtin_files[];
extern const int builtin_file_count;

// copy the built-in files to the disk, returns how many were copied or updated: missing files,
// apps still exactly as an older AerOS installed them (see /sys/apps.lst), and the manual
// whenever it differs. An app you have changed is left alone
int apps_install (void);

// how many files the last apps_install() couldn't write (a full disk or a drive error). Each one
// is simply tried again on the next boot
int apps_failed (void);

// the built-in copy of a file (e.g. "startup.wav") and its size, NULL if there is no such file
const void* apps_builtin (const char* name, uint32_t* size);

#endif /* APPS_H */

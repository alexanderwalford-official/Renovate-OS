/*
    apps.h
    The apps and files that come with AerOS. Their files (the apps folder) are built into the
    kernel and copied to the disk at boot (apps into /bin/, the startup sound into /sys/), so they
    are there even without a disk.
*/
#ifndef APPS_H
#define APPS_H

#include <stdint.h>

// copy every built-in file that isn't on the disk yet, returns how many were copied
// (a file already on the disk is left alone, so your own changes to it are kept)
int apps_install (void);

// the built-in copy of a file (e.g. "startup.wav") and its size, NULL if there is no such file
const void* apps_builtin (const char* name, uint32_t* size);

#endif /* APPS_H */

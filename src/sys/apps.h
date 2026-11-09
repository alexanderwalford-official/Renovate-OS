/*
    apps.h
    The apps and files that come with AerOS. Their files (src/apps and src/docs) are built into
    the kernel and copied to the disk at boot (apps into /bin/, the startup sound into /sys/ and
    the manual into /docs/), so they are there even without a disk.
*/
#ifndef APPS_H
#define APPS_H

#include <stdint.h>

// copy the built-in files to the disk, returns how many were copied or updated: missing files,
// apps still exactly as an older AerOS installed them (see /sys/apps.lst), and the manual
// whenever it differs. An app you have changed is left alone
int apps_install (void);

// the built-in copy of a file (e.g. "startup.wav") and its size, NULL if there is no such file
const void* apps_builtin (const char* name, uint32_t* size);

#endif /* APPS_H */

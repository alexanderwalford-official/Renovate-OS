/*
    apps.h
    The apps that come with AerOS. Their files (the apps folder) are built into the kernel and copied
    into /bin/ at boot, so they are there even without a disk.
*/
#ifndef APPS_H
#define APPS_H

// copy every built-in app that isn't in /bin/ yet, returns how many were copied
// (an app already on the disk is left alone, so your own changes to it are kept)
int apps_install (void);

#endif /* APPS_H */

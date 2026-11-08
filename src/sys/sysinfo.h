/*
    sysinfo.h
    Everything AerOS can find out about the computer it is running on: the processor, memory,
    screen, disks, sound card, every PCI device and AerOS itself.

    It comes out as rows of three strings, for example

        group "Processor"   name "Speed"   value "about 2400 MHz"

    so whoever shows it (the sysinfo app, /bin/sysinfo.aer, through the AerLang function
    system_info()) can lay it out however it likes. A value ending in "(NN%)" is a share of
    something, which the app draws as a bar.
*/
#ifndef SYSINFO_H
#define SYSINFO_H

typedef void (*sysinfo_fn)(const char* group, const char* name, const char* value, void* ctx);

// call fn once for every row, group by group. Takes a moment: it times the processor for 20ms
// and asks every IDE position whether a drive is there
void sysinfo_collect (sysinfo_fn fn, void* ctx);

#endif /* SYSINFO_H */

/*
    boot.h
    Everything the boot sequence knows about itself and the machine it is running on:
    - which boot option was picked (normal / fast / demo) from the boot options menu
    - what GRUB told us about the machine through the multiboot information structure
    - init_boot(), the boot log the user sees before the terminal starts

    The order things happen in (see kernel.c):
        boot_parse_multiboot() -> splash screen -> boot options menu (optional) -> init_boot() -> terminal
*/

#ifndef BOOT_H
#define BOOT_H

#include <stdint.h>
#include <stdbool.h>
#include "../drivers/video/VGA_linear.h"

// the version, shown by the terminal banner and the sysinfo app
#define AEROS_VERSION "0.01 ALPHA"

// boot options, chosen from the boot options menu (press X on the splash screen)
#define BOOT_OPT_NORMAL 0 // look for a AEROS disk (VFS if there isn't one) and show the boot log for a moment
#define BOOT_OPT_FAST   1 // skip the boot log delay (and the splash wait, when the command line says "fast")
#define BOOT_OPT_DEMO   2 // VFS mode, the disks are never touched
#define BOOT_OPT_RT     3 // a normal boot, plus the real-time tasks of RT mode (experimental, sys/rt.h)

extern int boot_option; // one of the BOOT_OPT_ values, BOOT_OPT_NORMAL unless the menu changed it
extern bool boot_options_requested; // set by the splash screen when X is pressed, kernel.c then opens the menu

// what the bootloader told us about the machine
extern bool boot_multiboot_valid; // true if a multiboot bootloader (GRUB) started us and gave us its info
extern bool boot_framebuffer_valid; // true if GRUB set up a graphics framebuffer (UEFI PCs)
extern vga_framebuffer_t boot_framebuffer; // where it is and its pixel format, for vga_init
extern uint32_t sys_mem_lower_kb; // KB of memory below 1MB
extern uint32_t sys_mem_upper_kb; // KB of memory above 1MB

// read the multiboot information GRUB left for us (magic and info_addr come from eax / ebx in entry.asm)
// fills in the globals above, does nothing if the magic number is wrong (not started by multiboot)
void boot_parse_multiboot (uint32_t magic, uint32_t info_addr);

// print the boot log, start the file system and (unless fast boot) pause so it can be read. Returns 0
int init_boot (void);

#endif /* BOOT_H */

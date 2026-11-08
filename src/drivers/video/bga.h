/*
    bga.h
    The Bochs Graphics Adapter: the simple graphics card QEMU, Bochs and VirtualBox emulate.
    Unlike real graphics cards it can change resolution with a few port writes, which lets
    AerOS switch modes while it is running. On real hardware the mode is picked at boot instead.
    https://wiki.osdev.org/Bochs_VBE_Extensions
*/
#ifndef BGA_H
#define BGA_H

#include <stdint.h>
#include <stdbool.h>

// true if the card is a Bochs Graphics Adapter
bool bga_available (void);

// switch to width x height at 32 bits per pixel and move the screen onto it
// false if there is no BGA, the mode is too big for its memory, or the screen isn't a framebuffer
bool bga_set_mode (uint32_t width, uint32_t height);

#endif /* BGA_H */

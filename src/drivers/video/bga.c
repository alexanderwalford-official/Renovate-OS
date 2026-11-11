/*
    bga.c - changing resolution on the Bochs Graphics Adapter

    The card has a handful of 16 bit registers behind an index port (0x1CE) and a data port
    (0x1CF): its ID, the width, height and colour depth, and an enable register. To change mode
    we switch the display off, write the new size, and switch it back on with the linear
    framebuffer enabled. The framebuffer stays at the same address, only its size changes.
*/

#include "bga.h"
#include "VGA_linear.h"
#include "../input/GIO.h"

#define BGA_INDEX 0x01CE
#define BGA_DATA  0x01CF

#define BGA_REG_ID        0x0
#define BGA_REG_XRES      0x1
#define BGA_REG_YRES      0x2
#define BGA_REG_BPP       0x3
#define BGA_REG_ENABLE    0x4
#define BGA_REG_VIDEO_64K 0xA // video memory in 64KB blocks (newer versions only)

#define BGA_ENABLED     0x01
#define BGA_LFB_ENABLED 0x40

// read one of the card's registers: its number goes to the index port, the value comes back
// through the data port (16 bit port reads and writes, as the registers are 16 bits)
static uint16_t bga_read (uint16_t reg) {
    outw(BGA_INDEX, reg);
    return inw(BGA_DATA);
}

// write one of the card's registers, the same way round
static void bga_write (uint16_t reg, uint16_t value) {
    outw(BGA_INDEX, reg);
    outw(BGA_DATA, value);
}

// is the graphics card a Bochs Graphics Adapter (QEMU, Bochs, VirtualBox), whose resolution we can change?
bool bga_available (void) {
    // the versions so far are 0xB0C0 to 0xB0C5. On any other card nothing answers at these ports
    // (a read usually gives 0xFFFF), so the ID is a safe way to ask. 0xB0CF leaves room for newer ones
    uint16_t id = bga_read(BGA_REG_ID);
    return id >= 0xB0C0 && id <= 0xB0CF;
}

// change the resolution to width x height at 32 bits a pixel, and move the console onto it
bool bga_set_mode (uint32_t width, uint32_t height) {
    vga_framebuffer_t fb;
    // only from a framebuffer (the card has to be in graphics mode already, and we need its
    // address), and nothing bigger than 4096 either way, a sensible limit for any screen
    if (!bga_available() || !vga_framebuffer_info(&fb) || width > 4096 || height > 4096) {
        return false;
    }
    // check the mode fits in the card's memory, when the card says how much it has
    // (4 bytes a pixel; older versions have no such register and read back 0)
    uint32_t memory = (uint32_t)bga_read(BGA_REG_VIDEO_64K) * 64 * 1024;
    if (memory != 0 && width * height * 4 > memory) {
        return false;
    }

    // the card only takes a new size while it is switched off
    bga_write(BGA_REG_ENABLE, 0);
    bga_write(BGA_REG_XRES, (uint16_t)width);
    bga_write(BGA_REG_YRES, (uint16_t)height);
    bga_write(BGA_REG_BPP, 32);
    // back on, with the linear framebuffer: the whole screen as one block of memory at the same
    // address as before, rather than the old 64KB window at 0xA0000
    bga_write(BGA_REG_ENABLE, BGA_ENABLED | BGA_LFB_ENABLED);

    // the card may round or refuse a size, so use what it actually set
    // its rows have no padding, so a row is just width * 4 bytes, and its 32 bit pixels are
    // 0x00RRGGBB: blue in the lowest byte, then green, then red
    fb.width = bga_read(BGA_REG_XRES);
    fb.height = bga_read(BGA_REG_YRES);
    fb.bpp = 32;
    fb.pitch = fb.width * 4;
    fb.red_pos = 16;
    fb.red_size = 8;
    fb.green_pos = 8;
    fb.green_size = 8;
    fb.blue_pos = 0;
    fb.blue_size = 8;
    return vga_change_framebuffer(&fb);
}

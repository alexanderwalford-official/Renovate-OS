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

static uint16_t bga_read (uint16_t reg) {
    outw(BGA_INDEX, reg);
    return inw(BGA_DATA);
}

static void bga_write (uint16_t reg, uint16_t value) {
    outw(BGA_INDEX, reg);
    outw(BGA_DATA, value);
}

bool bga_available (void) {
    // the versions so far are 0xB0C0 to 0xB0C5
    uint16_t id = bga_read(BGA_REG_ID);
    return id >= 0xB0C0 && id <= 0xB0CF;
}

bool bga_set_mode (uint32_t width, uint32_t height) {
    vga_framebuffer_t fb;
    if (!bga_available() || !vga_framebuffer_info(&fb) || width > 4096 || height > 4096) {
        return false;
    }
    // check the mode fits in the card's memory, when the card says how much it has
    uint32_t memory = (uint32_t)bga_read(BGA_REG_VIDEO_64K) * 64 * 1024;
    if (memory != 0 && width * height * 4 > memory) {
        return false;
    }

    bga_write(BGA_REG_ENABLE, 0);
    bga_write(BGA_REG_XRES, (uint16_t)width);
    bga_write(BGA_REG_YRES, (uint16_t)height);
    bga_write(BGA_REG_BPP, 32);
    bga_write(BGA_REG_ENABLE, BGA_ENABLED | BGA_LFB_ENABLED);

    // the card may round or refuse a size, so use what it actually set
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

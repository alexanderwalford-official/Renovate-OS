/*
    Renovate OS
    Developed by Alexander Walford for Renovate Software LTD 2023

    X86 32 bit protected mode, loaded by GRUB (multiboot)
*/

#include <stdint.h>
#include "drivers/input/GIO.h"
#include "drivers/input/IO.h"
#include "drivers/serial/serial.h"
#include "drivers/video/VGA_linear.h"
#include "sys/interrupts.h"
#include "sys/sys_clck.h"
#include "sys/p_man.h"
#include "sys/boot.h"
#include "sys/recovery.h"

// called from entry.asm with the multiboot information address and magic number GRUB left in ebx / eax
int main(uint32_t mb_info_addr, uint32_t mb_magic) {

    // hardware setup, this has to happen before any process runs
    vga_init();                                   // screen & cursor
    serial_init();                                // debug output on COM1
    boot_parse_multiboot(mb_magic, mb_info_addr); // memory size
    gdt_init();                                   // segments
    idt_init();                                   // exception & IRQ handlers, PIC remap
    clck_init();                                  // 1000Hz system timer (IRQ0)
    init_keyboard();                              // PS/2 keyboard (IRQ1)
    interrupts_enable();

    int pid;

    // processes for startup
    pid = p_create("splash_screen");
    p_exec(pid);
    p_destroy(pid);

    // if X was pressed on the splash screen, open the boot options
    if (boot_options_requested) {
        init_recovery();
    }

    pid = p_create("init_boot");
    p_exec(pid);
    p_destroy(pid);

    // the terminal runs until the computer is turned off
    pid = p_create("terminal");
    p_exec(pid);
    p_destroy(pid);

    return 0;
}

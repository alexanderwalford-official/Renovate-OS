/*
    power.h
    Reboot, shut down and halt the machine.
*/
#ifndef POWER_H
#define POWER_H

#include <stdint.h>
#include "../drivers/input/GIO.h"
#include "../drivers/video/VGA_linear.h"

// stop the CPU forever
void sys_halt(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

// restart the computer
void sys_reboot(void) {
    interrupts_disable();

    // ask the keyboard controller to pulse the CPU reset line
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++) {
    }
    outb(0x64, 0xFE);

    // if that did not work, force a triple fault with an empty IDT
    struct __attribute__((packed)) { uint16_t limit; uint32_t base; } null_idt = { 0, 0 };
    __asm__ volatile ("lidt %0; int $3" : : "m"(null_idt));
    sys_halt();
}

// power off the computer (emulators only for now, real hardware needs ACPI support)
void sys_shutdown(void) {
    interrupts_disable();
    outw(0x604, 0x2000);  // QEMU
    outw(0xB004, 0x2000); // Bochs and older QEMU
    outw(0x4004, 0x3400); // VirtualBox

    // still running: tell the user it is safe to switch off
    clear(BLACK);
    puts(20, 12, WHITE, BLACK, "It is now safe to turn off your computer.");
    sys_halt();
}

#endif /* POWER_H */

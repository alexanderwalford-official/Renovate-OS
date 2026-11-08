/*
    power.c
    Reboot, shut down and halt the machine, see power.h.

    None of this has a single clean "do it" instruction on a PC, so each function uses the
    traditional tricks:

      halt      "cli" stops hardware interrupts, "hlt" sleeps until the next interrupt. With both
                together the CPU sleeps for good. A non maskable interrupt (NMI) can still wake it,
                which is why it sits in a loop and goes straight back to sleep.

      reboot    the 8042 PS/2 keyboard controller has an output wired to the CPU's reset pin (an
                IBM AT design quirk that every PC kept). Command 0xFE pulses it. If that does not
                work we crash the CPU on purpose with a triple fault, which makes the motherboard
                (or emulator) reset it.

      shutdown  real hardware needs ACPI: finding the FADT and the \_S5 sleep values in the AML
                bytecode of the DSDT table, which AerOS does not do yet. Emulators however
                answer at fixed I/O ports, so we just try those.

    https://wiki.osdev.org/Reboot
    https://wiki.osdev.org/Shutdown
    https://wiki.osdev.org/Triple_Fault
*/
#include "power.h"
#include <stdint.h>
#include "../drivers/input/GIO.h"
#include "../drivers/video/VGA_linear.h"

// stop the CPU forever, see power.h
void sys_halt(void) {
    // loop because an NMI (or the firmware's SMI) can still wake the CPU out of hlt
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

// restart the computer, see power.h
void sys_reboot(void) {
    // nothing must interrupt us half way through, and the triple fault trick below relies on it
    interrupts_disable();

    // ask the keyboard controller to pulse the CPU reset line
    // port 0x64 is the 8042's status register (read) and command register (write). Status bit 1
    // (0x02) means "input buffer full": the controller has not taken the last byte yet, so wait
    // for it to clear (with a limit, in case there is no controller at all) before sending.
    // Command 0xFE = 1111 1110: the low 4 bits pick which output lines to pulse low, a 0 means
    // pulse. Only line 0 is pulsed, and line 0 is the CPU reset
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++) {
    }
    outb(0x64, 0xFE);

    // if that did not work, force a triple fault with an empty IDT
    // with an IDT limit of 0 there is no gate for any vector. "int $3" (breakpoint) cannot find
    // gate 3, so the CPU raises a general protection fault, which has no gate either, so it
    // raises a double fault, which has no gate either: that is a triple fault, and the CPU
    // gives up and resets. (QEMU started with -no-reboot just stops instead.)
    // packed for the same reason as table_ptr_t in interrupts.c: lidt wants exactly 6 bytes
    struct __attribute__((packed)) { uint16_t limit; uint32_t base; } null_idt = { 0, 0 };
    __asm__ volatile ("lidt %0; int $3" : : "m"(null_idt));
    // never reached, but the compiler is promised this function does not return
    sys_halt();
}

// power off the computer, see power.h
void sys_shutdown(void) {
    interrupts_disable();
    // each of these writes the emulator's ACPI "PM1a control" register: bit 13 (0x2000) is
    // SLP_EN, "go to sleep now", and bits 10-12 are the sleep type, which is the soft off (S5)
    // state for that emulator: type 0 on QEMU / Bochs, type 5 on VirtualBox (5 << 10 = 0x1400,
    // plus 0x2000 = 0x3400). On real hardware these ports are normally unused and nothing happens
    outw(0x604, 0x2000);  // QEMU
    outw(0xB004, 0x2000); // Bochs and older QEMU
    outw(0x4004, 0x3400); // VirtualBox

    // still running: tell the user it is safe to switch off
    // (column 20, row 12 roughly centres the 41 character message on the 80 x 25 text screen)
    clear(BLACK);
    puts(20, 12, WHITE, BLACK, "It is now safe to turn off your computer.");
    sys_halt();
}

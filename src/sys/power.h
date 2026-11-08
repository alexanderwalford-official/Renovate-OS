/*
    power.h
    Reboot, shut down and halt the machine.

    Every function here is marked noreturn: once called, the kernel never gets control back,
    which also lets the compiler skip generating code for "after" the call.

      sys_halt()      park the CPU forever (used after a crash and as the last resort of the others)
      sys_reboot()    reset the machine through the keyboard controller, or a deliberate triple fault
      sys_shutdown()  power off, which without ACPI only works in emulators (QEMU, Bochs, VirtualBox)

    https://wiki.osdev.org/Reboot
    https://wiki.osdev.org/Shutdown
*/
#ifndef POWER_H
#define POWER_H

// stop the CPU forever
// (interrupts off, then hlt in a loop, so not even an NMI can get the CPU going again for long)
__attribute__((noreturn)) void sys_halt(void);

// restart the computer
// (pulses the reset line through the 8042 keyboard controller, falling back to a triple fault)
__attribute__((noreturn)) void sys_reboot(void);

// power off the computer (emulators only for now, real hardware needs ACPI support)
// if nothing switches off, it shows "It is now safe to turn off your computer." and halts
__attribute__((noreturn)) void sys_shutdown(void);

#endif /* POWER_H */

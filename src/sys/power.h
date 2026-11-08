/*
    power.h
    Reboot, shut down and halt the machine.
*/
#ifndef POWER_H
#define POWER_H

// stop the CPU forever
__attribute__((noreturn)) void sys_halt(void);

// restart the computer
__attribute__((noreturn)) void sys_reboot(void);

// power off the computer (emulators only for now, real hardware needs ACPI support)
__attribute__((noreturn)) void sys_shutdown(void);

#endif /* POWER_H */

/*
    recovery.h
    The boot options (recovery) menu, opened by pressing X on the splash screen.
    It lets the user choose how the rest of the boot goes (normal, fast or demo mode, see
    BOOT_OPT_ in boot.h), or reboot / shut down before anything touches the disks.
*/

#ifndef RECOVERY_H
#define RECOVERY_H

// run the boot options menu as its own process ("iso_launch_options"), returns 0 or -1 if the process stack is full
int init_recovery (void);

// draw the boot options screen and wait for 1 - 5, sets boot_option (or reboots / shuts down)
// returns 0 once an option is picked, or -1 straight away if there is no keyboard
int boot_options_menu (void);

#endif /* RECOVERY_H */

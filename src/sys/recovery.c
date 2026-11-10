/*
    recovery.c - the boot options menu

    If X is pressed during the splash screen, kernel.c calls init_recovery() before the
    boot log runs. The menu is run as a process like every other boot step, so it goes
    through the process manager and the AerLang compiler ("iso_launch_options").

    The menu only sets boot_option, it is init_boot() (sys/boot.c) that acts on it:
        [1] normal -> BOOT_OPT_NORMAL: use a AEROS disk if there is one, show the boot log
        [2] fast   -> BOOT_OPT_FAST:   the same, but skip the pause after the boot log
        [3] demo   -> BOOT_OPT_DEMO:   never touch the disks, files only live in memory (VFS)
        [4] reboot and [5] shut down happen straight away and never come back
        [6] RT mode -> BOOT_OPT_RT:   a normal boot, plus real-time tasks (experimental, sys/rt.h)
*/

#include "recovery.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/input/IO.h"
#include "boot.h"
#include "power.h"
#include "p_man.h"

// the boot options screen (opened by pressing X on the splash screen)
// drawn in the same black on light grey style as the splash screen
// returns 0 once 1, 2, 3 or 6 is picked, -1 if there is no keyboard (the boot then carries on as normal)
// gotcha: it can also be opened later with "run iso_launch_options", but by then the file system is
// already set up, so a new choice only changes boot_option (which can switch the terminal's VFS / DEMO banner)
int boot_options_menu (void) {
    clear(BRIGHT);
    puts(27, 5, BLACK, BRIGHT, "______________________");
    puts(32, 7, BLACK, BRIGHT, "BOOT OPTIONS");
    puts(27, 8, BLACK, BRIGHT, "______________________");
    puts(25, 11, BLACK, BRIGHT, "[1] Continue normal boot");
    puts(25, 12, BLACK, BRIGHT, "[2] Fast boot (skip boot log)");
    puts(25, 13, BLACK, BRIGHT, "[3] Demo mode (VFS, no disks)");
    puts(25, 14, BLACK, BRIGHT, "[4] Reboot");
    puts(25, 15, BLACK, BRIGHT, "[5] Shut down");
    puts(25, 16, BLACK, BRIGHT, "[6] RT mode (experimental)");
    puts(25, 18, BLACK, BRIGHT, "< PRESS 1 - 6 TO CHOOSE >");

    // throw away anything still waiting from the splash screen, so a stray key isn't taken as a choice
    ClearInputbuffer();
    // keep reading keys until one of them is a valid choice
    for (;;) {
        // getk() waits for the next key press
        switch (getk()) {
            case '1': boot_option = BOOT_OPT_NORMAL; return 0;
            case '2': boot_option = BOOT_OPT_FAST; return 0;
            case '3': boot_option = BOOT_OPT_DEMO; return 0;
            // sys_reboot() and sys_shutdown() never return, the "return 0" just keeps each case tidy
            case '4': sys_reboot(); return 0;
            case '5': sys_shutdown(); return 0;
            case '6': boot_option = BOOT_OPT_RT; return 0;
            case 0: return -1; // no keyboard available
            default: break; // any other key, ask again
        }
    }
}

// start the boot options menu
// it goes through the process manager like the rest of the boot: create, execute, destroy
// returns 0 when the menu has finished, or -1 if there was no free slot on the process stack
int init_recovery (void) {
    // process for launch options
    int pid = p_create("iso_launch_options");
    if (pid < 0) {
        return -1;
    }
    p_exec(pid);
    p_destroy(pid);
    return 0;
}

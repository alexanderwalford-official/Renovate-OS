#include "recovery.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/input/IO.h"
#include "boot.h"
#include "power.h"
#include "p_man.h"

// the boot options screen (opened by pressing X on the splash screen)
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
    puts(25, 18, BLACK, BRIGHT, "< PRESS 1 - 5 TO CHOOSE >");

    ClearInputbuffer();
    for (;;) {
        switch (GetChar()) {
            case '1': boot_option = BOOT_OPT_NORMAL; return 0;
            case '2': boot_option = BOOT_OPT_FAST; return 0;
            case '3': boot_option = BOOT_OPT_DEMO; return 0;
            case '4': sys_reboot(); return 0;
            case '5': sys_shutdown(); return 0;
            case 0: return -1; // no keyboard available
            default: break;
        }
    }
}

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

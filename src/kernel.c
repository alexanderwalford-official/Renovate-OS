#include "stdint.h"
#include "drivers/video/IO.h"
#include "drivers/input/PS2.h"
#include "drivers/input/mouse.h"
#include "sys/p_man.h"
#include "sys/f_sys.h"
#include "sys/boot.h"
#include "sys/recovery.h"

/*
TEMP DISABLED ASM CALL:
#include "asm/primary_handler.asm"
extern PrimaryHandler();
*/

int main() {
    clear(BRIGHT);
    // COL : ROW
    puts(26, 7, BLACK, BRIGHT, "______________________________");
    puts(35, 8, BLACK, BRIGHT, "");
    puts(35, 9, BLACK, BRIGHT, "Renovate OS");
    puts(28, 10, BLACK, BRIGHT, "Renovate Software LTD 2022");
    puts(26, 11, BLACK, BRIGHT, "______________________________");
    puts(31, 18, BLACK, BRIGHT, "< F2 FOR RECOVERY >");

    // untested method using process:
    p_create("[say:Renovate OS \nRenovate Software LTD 2022 \nF2 To Enter Recovery|3510|01]");
    p_exec(0); 
    (ps2_get_char() == "F2") ? init_recovery() : ret(); // if presses F2, open recovery
    p_freeze(0, 4, 0); // wait 4 seconds

    // initalise keyboard scanning
    init_ps2();

    // initialise boot seq
    // init_boot(); // temp disabled to test the boot screen

    return 0;
}

void ret () {
    return;
}
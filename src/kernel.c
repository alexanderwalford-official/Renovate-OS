#include "stdint.h"
#include "lib/stdio.h"
#include "drivers/video/put.h"
#include "drivers/input/keyboard.h"
#include "drivers/input/mouse.h"
#include "sys/p_man.h"
#include "sys/f_sys.h"

/*
TEMP DISABLED ASM CALL:
#include "asm/primary_handler.asm"
extern PrimaryHandler();
*/

int main() {
    clear(BRIGHT);
    // COL : ROW
    puts(35, 10, BLACK, BRIGHT, "Renovate OS");
    puts(28, 11, BLACK, BRIGHT, "Renovate Software LTD 2022");

    // untested:
    p_create("[say:Renovate OS|3510|01]"); // this is where we would print the text above
    p_exec(0); // execute process via ID
    p_freeze(0, 4, 0); // this is where we could freeze the boot screen process before we load the main methods

    // initialise the temproary file system
    fs_init("n");

    return 0;
}

void ret () {
    return;
}
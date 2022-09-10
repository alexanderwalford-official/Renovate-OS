#include "stdint.h"
#include "drivers/video/IO.h"
#include "drivers/input/PS2.h"
#include "drivers/input/USB.h"
#include "sys/p_man.h"
#include "sys/f_sys.h"
#include "sys/boot.h"
#include "sys/recovery.h"
#include "sys/terminal/t_int.h"
#include "lib/primary_definitions.h"

/*
    TEMP DISABLED ASM CALL:
    #include "asm/primary_handler.asm"
    extern PrimaryHandler();
*/

int main() {
    // set the screen background colour
    clear(BRIGHT);

    // init PS2 device scanning
    init_ps2();

    // init USB device scanning
    init_USB();

    // process management for startup screen
    p_create("say:Renovate OS \nRenovate Software LTD 2022 \n< F2 FOR RECOVERY >|3510|01");
    p_exec(0); 
    (ps2_get_char() == "F2") ? init_recovery() : ret(); // if presses F2, open recovery
    p_freeze(0, 4, 0); // wait 4 seconds
    p_destory(0); // destory the process

    // create new process for boot sequence
    p_create("init_boot");
    p_exec(0); // then execute it

    return 0;
}
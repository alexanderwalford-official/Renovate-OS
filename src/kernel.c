/*
    Renovate OS
    Developed by Alexander Walford for Renovate Software LTD 2022

    X86 Real Mode
*/

#include "stdint.h"
#include "drivers/video/VGA_linear.h"
#include "drivers/input/PS2.h"
#include "drivers/input/USB.h"
#include "sys/p_man.h"
#include "sys/f_sys.h"
#include "sys/boot.h"
#include "sys/recovery.h"
#include "sys/terminal/t_int.h"
#include "lib/primary_definitions.h"
#include "drivers/audio/tone.h"

int main() {

    // init PS2 device scanning
    init_ps2();

    // init USB device scanning
    init_USB();

    // processes for startup
    p_create("splash_screen");
    p_exec(0);
    beep(0);
    (ps2_get_char() == "F2") ? init_recovery() : ret(); // if presses F2, open recovery
    p_create("init_boot");
    p_freeze(1, 4000);
    p_destory(0);
    p_exec(0);
    p_destory(0);

    return 0;
}
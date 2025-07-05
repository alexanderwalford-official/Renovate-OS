/*
    Renovate OS
    Developed by Alexander Walford for Renovate Software LTD 2023

    X86 Real Mode
*/

#include "stdint.h"
#include "drivers/video/VGA_linear.h"
#include "drivers/input/IO.h"
#include "drivers/audio/tone.h"
#include "sys/p_man.h"
#include "sys/f_sys.h"
#include "sys/boot.h"
#include "sys/recovery.h"
#include "sys/terminal/t_int.h"
#include "lib/primary_definitions.h"

int main() {
    // processes for startup
    p_create("splash_screen");
    p_exec(0);
    p_freeze(0, 4000);
    p_destory(0);

    //(GetChar() == "x") ? init_recovery() : ret(); // if presses x, open recovery
    p_create("init_boot");
    p_exec(1);
    p_freeze(1, 4000);
    p_destory(1);

    p_create("print_stack");
    p_exec(2);
    p_freeze(2, 4000);
    p_destory(2);
    
    return 0;
}
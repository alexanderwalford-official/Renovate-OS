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

    // NOTE: p_destroy does not seem to be working as intended with p_create

    // processes for startup
    p_create("splash_screen");
    p_exec(0);
    p_destory(0);

    //(GetChar() == "x") ? init_recovery() : ret(); // if presses x, open recovery
    
    p_create("init_boot");
    p_exec(0);
    p_destory(0);

    p_create("flash");
    p_exec(0);
    p_destory(0);

    p_create("print_stack");
    p_exec(0);
    p_freeze(0, 4000);
    p_destory(0);

    p_create("error");
    p_exec(0);
    p_destory(0);
    
    return 0;
}
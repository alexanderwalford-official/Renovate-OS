#include "stdint.h"
#include "../drivers/video/VGA_linear.h"

int init_recovery () {
    // process for launch options
    p_create("iso_launch_options");
    p_exec(0);
    p_freeze(0, 4000, 0);
    p_destory(0);
    return 0;
}
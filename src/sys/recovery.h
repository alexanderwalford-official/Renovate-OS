#include "stdint.h"
#include "../drivers/video/VGA_linear.h"

int init_recovery () {
    clear(BRIGHT);
    puts(0, 0, BLACK, BRIGHT, "ENTERING RECOVERY MODE..");
    return 0;
}
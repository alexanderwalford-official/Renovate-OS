#include "stdint.h"
#include "../drivers/video/IO.h"

int init_recovery () {
    clear(BRIGHT);
    puts(0, 0, BLACK, BRIGHT, "ENTERING RECOVERY MODE..");
    return 0;
}
#include "../drivers/video/IO.h"
#include "f_sys.h"


int init_boot () {
    // clear screen
    clear(BRIGHT);
    
    // initialise the temproary file system
    fs_init("n");

    return 0;
}
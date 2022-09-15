#ifndef BOOT_H
#define BOOT_H

#include "../drivers/video/IO.h"
#include "f_sys.h"
#include "../sys/terminal/t_int.h"
#include "../drivers/audio/tone.h"


int init_boot () {

    // clear screen
    clear(BRIGHT);
    
    // initialise the temproary file system
    fs_init("n");
    
    // initialise the terminal, bypass login
    cli_init(0);

    // beep once
    beep(0);

    return 0;
}

#endif /* BOOT_H */
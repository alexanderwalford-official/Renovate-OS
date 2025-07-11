#ifndef BOOT_H
#define BOOT_H

#include "../drivers/video/VGA_linear.h"
#include "f_sys.h"
#include "../sys/terminal/t_int.h"
#include "../drivers/audio/tone.h"
#include "../sys/sys_clck.h"


int init_boot () {

    // clear screen
    clear(BRIGHT);

    puts(0, 2, BRIGHT, BLACK, "[E:b1f1]");
    
    // initialise the temproary file system
    fs_init("n");

    // wait
    clck_hang(4000, 0);
    
    // initialise the terminal, bypass login
    cli_init(0);

    // beep once
    //beep(0);

    puts(0, 20, BRIGHT, BLACK, "[VFS:UP]");

    return 0;
}

#endif /* BOOT_H */
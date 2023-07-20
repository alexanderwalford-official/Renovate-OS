// terminal interface

#ifndef T_INT_H
#define T_INT_H

#include "stdint.h"
#include "../../drivers/video/VGA_linear.h"
#include "../../drivers/input/IO.h"

int cli_init (int ent) {
    if (ent == 0) {
        // bypass login
        cli_rend_main();
    }
    return 0;
}

void cli_rend_main () {
    clear(BLACK);
    puts(0, 0, BRIGHT, BLACK, "Renovate OS - Version 0.01 ALPHA");
    puts(0, 1, BRIGHT, BLACK, "Renovate Software LTD 2023");
    puts(0, 3, BRIGHT, BLACK, "0//: ");
    InputBufferChangeState(1);
    puts(5, 3, BRIGHT, BLACK, InputBuffer);
    HandleInput();
    p_freeze(1, 1000);
    InputBufferChangeState(0);
    cli_rend_main();
}

void HandleInput () {
    if (InputBuffer != "") {
        if (InputBuffer == "help") {
        puts(0, 4, BRIGHT, BLACK, "> HELP: ");
        puts(0, 5, BRIGHT, BLACK, "HELP - THIS COMMAND");
        puts(0, 6, BRIGHT, BLACK, "LA - LIST ALL FILES AND DIRECTORIES IN THE CURRENT DIRECTORY");
        puts(0, 7, BRIGHT, BLACK, "SD - SET THE CURRENT DIRECTORY");
        puts(0, 8, BRIGHT, BLACK, "CLK - PRINTS THE SYSTEM TIME");
        }
        else if (InputBuffer == "pman") {
            puts(0, 4, BRIGHT, BLACK, "> PMAN: ");
            puts(0, 5, BRIGHT, BLACK, "Feature coming soon!");
        }
        else {
            // unrecognised
        }
    }
    else {
        // buffer is empty
        p_freeze(1, 1000);
        HandleInput();
    }
}

#endif /* T_INT_H */
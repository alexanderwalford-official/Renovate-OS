// terminal interface

#ifndef T_INT_H
#define T_INT_H

#include "stdint.h"
#include "../../drivers/video/VGA_linear.h"
#include "../../drivers/input/IO.h"
#include "../../lib/primary_definitions.h"
#include "../sys_clck.h"


int flash_pointer (int times, int lc) {
    if (lc < times) {
        puts(0, 6, BRIGHT, BLACK, " ");
        clck_hang(2000, 0);
        puts(0, 6, BLACK, BRIGHT, " ");
        clck_hang(2000, 0);
        lc = lc + 1;
        flash_pointer(times, lc);
    }
    else {
        return 0;
    }
}

int cli_init (int ent) {
    if (ent == 0) {
        // bypass login
        cli_rend_main();
        flash_pointer(2, 0);
    }
    return 0;
}

int cli_rend_main () {
    clear(BLACK);
    puts(0, 0, WHITE, BLACK, "Renovate OS - Version 0.01 ALPHA");
    puts(0, 1, WHITE, BLACK, "Alexander Walford 2025");
    puts(0, 3, RED, BLACK, "[ ! ] You are in VFS mode, your files will not be written.");
    puts(0, 5, BRIGHT, BLACK, "0//: ");
    ClearInputbuffer();
    InputBufferChangeState(1);
    init_keyboard(); // problematic
    //HandleInput();
    return 0;
}

void HandleInput () {
    char in[] = "";
    char new_in = GetString(in);
    puts(5, 3, BRIGHT, BLACK, new_in);
    if (new_in != "") {
        if (new_in == '\n') {
            if (StrCompare(new_in, "help")) {
                puts(0, 4, BRIGHT, BLACK, "> HELP: ");
                puts(0, 5, BRIGHT, BLACK, "HELP - THIS COMMAND");
                puts(0, 6, BRIGHT, BLACK, "LA - LIST ALL FILES AND DIRECTORIES IN THE CURRENT DIRECTORY");
                puts(0, 7, BRIGHT, BLACK, "SD - SET THE CURRENT DIRECTORY");
                puts(0, 8, BRIGHT, BLACK, "CLK - PRINTS THE SYSTEM TIME");
                p_freeze(1, 1000);
                ClearInputbuffer();
                cli_rend_main();
            }
            else if (StrCompare(new_in, "pman")) {
                puts(0, 4, BRIGHT, BLACK, "> PMAN: ");
                puts(0, 5, BRIGHT, BLACK, "Feature coming soon!");
                p_freeze(1, 1000);
                ClearInputbuffer();
                cli_rend_main();
            }
            else if (StrCompare(new_in, "time")) {
                puts(0, 4, BRIGHT, BLACK, "> TIME: ");
                puts(0, 5, BRIGHT, BLACK, "Feature coming soon!");
                p_freeze(1, 1000);
                ClearInputbuffer();
                cli_rend_main();
            }
            else {
                // unrecognised
                puts(0, 4, BRIGHT, BLACK, "> ");
                puts(3, 4, BRIGHT, BLACK, new_in);
                puts(0, 5, BRIGHT, BLACK, "Unrecognised command, please try again.");
                p_freeze(1, 1000);
                ClearInputbuffer();
                cli_rend_main();
            }
        }
    }
    else {
        // buffer is empty
        p_freeze(1, 1000);
        HandleInput(in);
    }
}

#endif /* T_INT_H */
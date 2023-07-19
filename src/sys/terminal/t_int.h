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

char* CombineInput (int c_cnt) {
    c_cnt = c_cnt + 1;
    char* c_comb[] = {}; // blank string array
    char c = GetChar(); // get a single character
    (c == "") ? GetChar() : (c != "\n") ? c_comb[c_cnt] = c : 0;
    return c_comb;
}

void cli_rend_main () {
    clear(BLACK);
    puts(0, 0, BRIGHT, BLACK, "Renovate OS - Version 0.01 ALPHA");
    puts(0, 1, BRIGHT, BLACK, "Renovate Software LTD 2022");
    puts(0, 3, BRIGHT, BLACK, "0//: ");

    puts(5, 3, BRIGHT, BLACK, CombineInput);

    if (CombineInput == "help") {
        puts(0, 4, BRIGHT, BLACK, "> HELP: ");
        puts(0, 5, BRIGHT, BLACK, "HELP - THIS COMMAND");
        puts(0, 6, BRIGHT, BLACK, "LA - LIST ALL FILES AND DIRECTORIES IN THE CURRENT DIRECTORY");
        puts(0, 7, BRIGHT, BLACK, "SD - SET THE CURRENT DIRECTORY");
        puts(0, 8, BRIGHT, BLACK, "CLK - PRINTS THE SYSTEM TIME");
    }
}
#endif /* T_INT_H */
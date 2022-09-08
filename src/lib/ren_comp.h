// RenLang for Renovate OS compiler
#include "stdint.h"
#include "../drivers/video/IO.h"
#include "../lib/sys_primary_definitions.h"
#include "stdbool.h"

/*
    COLOR GUIDE FOR PROGRAMMING INTERFACE:
    0 = BLACK
    1 = BRIGHT
*/


// pass arguments to compiler
int comp_handler (char* arg) {
    return 0;
}

// dynamic variables
int c_lc = 0; // compiler line counter
bool c_say = false;
bool c_say_align = false;
bool c_say_col = false;
char* c_str_to_say = "";
int c_i_say_align[1][1] = {{0}, {0}};
char* c_say_cols[1][1] = {{""},{""}};

// main method to compile input line code
void compile (char* code) {
    
    if (c_say) {
        c_say = false;
        c_str_to_say = code;
        c_say_align = true;
    }
    else if (c_say_align) {
        c_say_align = false;
        c_i_say_align[0][0] = code[0] + code[1];
        c_i_say_align[0][0] = code[2] + code[3];
        c_say_col = true;
    }
    else if (c_say_col) {
        c_say_col = false;
        c_say_cols[0][0] = code[0];
        c_say_cols[0][0] = code[1];
        enum color c1;
        enum color c2;
        if (c_say_cols[0] == "0") {
            c1 = BLACK;
        }
        else {
            c1 = BRIGHT;
        }
        if (c_say_cols[1] == "0") {
            c2 = BLACK;
        }
        else {
            c2 = BRIGHT;
        }

        puts(c_i_say_align[0], c_i_say_align[1], c1, c2, c_str_to_say);
    }

    (contains(code, "say:", 0)) ? c_say = true : 0;

    c_lc = c_lc + 1;
}
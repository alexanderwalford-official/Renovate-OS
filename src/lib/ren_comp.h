#ifndef REN_COMP_H
#define REN_COMP_H

// RenLang for Renovate OS compiler
#include "stdint.h"
#include "stdbool.h"
#include "../drivers/video/VGA_linear.h"
#include "../lib/primary_definitions.h"
#include "../sys/p_man.h"
#include "../sys/boot.h"
#include "../sys/sys_clck.h"
#include "../sys/terminal/t_int.h"


/*
    COLOUR GUIDE FOR PROGRAMMING INTERFACE:
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
char* c_str_to_say;
int c_i_say_align[1][1] = {{0}, {0}};
char* c_say_cols[1][1] = {{""},{""}};

int splash_screen () {
    clear(BRIGHT);
    puts(27, 7, BLACK, BRIGHT, "______________________");
    puts(33, 9, BLACK, BRIGHT, "Renovate OS");
    puts(28, 10, BLACK, BRIGHT, "Alexander Walford 2025");
    puts(27, 11, BLACK, BRIGHT, "______________________");
    puts(33, 13, BLACK, BRIGHT, "V 0.1 ALPHA");
    puts(27, 18, BLACK, BRIGHT, "< X FOR BOOT OPTIONS >");

    clck_hang(4000, 0);

    return 0;
}

int error_screen () {
    clear(RED);
    puts(5, 7, BLACK, RED, "==================================");
    puts(5, 9, BLACK, RED, ">> SYSTEM EXCEPTION <<");
    puts(5, 10, BLACK, RED, "The system encountered an error and was forced to stop.");
    puts(5, 11, BLACK, RED, "Message is as followed:");
    puts(5, 12, BLACK, RED, "[ ! ] OS COMPILER LEVEL EXCEPTION >> INVALID CODE PARSED");
    // wait and load the OS again by re-creating the relevant processes
    clck_hang(4000, 0);
    return 0;
}

// main method to compile input line code
int compile (char* code) {

    // the following is for printing text

    // // check if contains "s:"
    // if (contains_str(code, "s:")) {
    //     c_say = true;
    //     compile(remove(code, "s:", 0));
    // }
    // // check if say active
    // else if (c_say) {
    //     c_say = false;
    //     c_str_to_say = remove(code, "s:", 0);
    //     c_say_align = true;
    //     // temp
    //     puts(0, 0, BLACK, BRIGHT, c_str_to_say);
    //     //compile(c_str_to_say);
    // }
    // else if (c_say_align) {
    //     c_say_align = false;
    //     c_i_say_align[0][0] = code[0] + code[1];
    //     c_i_say_align[0][0] = code[2] + code[3];
    //     c_say_col = true;
    //     compile(code);
    // }
    // else if (c_say_col) {
    //     c_say_col = false;
    //     c_say_cols[0][0] = code[0];
    //     c_say_cols[0][0] = code[1];
        
    //     enum color c1;
    //     enum color c2;

    //     if (c_say_cols[0] == "0") {
    //         c1 = BLACK;
    //     }
    //     else {
    //         c1 = BRIGHT;
    //     }
    //     if (c_say_cols[1] == "0") {
    //         c2 = BLACK;
    //     }
    //     else {
    //         c2 = BRIGHT;
    //     }

    //     puts(c_i_say_align[0], c_i_say_align[1], c1, c2, c_str_to_say);
    // }



    // text printing ends here
    if (StrCompare(code, "update_stack") == 1) {
        p_stack_update(0);
    }
    else if (StrCompare(code, "init_boot") == 1) {
        init_boot();
    }
    else if (StrCompare(code, "splash_screen") == 1) {
        // print the splash screen
        splash_screen();
    }
    else if (StrCompare(code, "print_stack")) {
        // print (dmp) the process stack
        stack_print();
    }
    else if (StrCompare(code, "flash")) {
        flash_pointer(2, 0);
    }
    else {
        // invalid code
        error_screen();
    }

    c_lc = c_lc + 1;

    return 0;
}

#endif /* REN_COMP_H */
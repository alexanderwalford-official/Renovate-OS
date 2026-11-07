#ifndef REN_COMP_H
#define REN_COMP_H

// RenLang for Renovate OS compiler
#include <stdint.h>
#include <stdbool.h>
#include "../drivers/video/VGA_linear.h"
#include "../drivers/input/IO.h"
#include "../drivers/audio/tone.h"
#include "../lib/primary_definitions.h"
#include "../sys/sys_clck.h"
#include "../sys/boot.h"

// prototypes (ren_comp.h, p_man.h, t_int.h and recovery.h use each other)
int comp_handler (char* arg);
int splash_screen (void);
int error_screen (const char* code);
int compile (const char* code);

#include "../sys/p_man.h"
#include "../sys/terminal/t_int.h"
#include "../sys/recovery.h"

/*
    COLOUR GUIDE FOR PROGRAMMING INTERFACE:
    0 = BLACK
    1 = BRIGHT
*/

// pass arguments to compiler
int comp_handler (char* arg) {
    (void)arg;
    return 0;
}


// dynamic variables
int c_lc = 0; // compiler line counter

int splash_screen () {
    clear(BRIGHT);
    puts(27, 7, BLACK, BRIGHT, "______________________");
    puts(33, 9, BLACK, BRIGHT, "Renovate OS");
    puts(28, 10, BLACK, BRIGHT, "Alexander Walford 2026");
    puts(27, 11, BLACK, BRIGHT, "______________________");
    puts(33, 13, BLACK, BRIGHT, "V 0.1 ALPHA");
    puts(27, 18, BLACK, BRIGHT, "< X FOR BOOT OPTIONS >");

    // wait up to 4 seconds, pressing X opens the boot options
    ClearInputbuffer();
    uint32_t start = clck_uptime_ms();
    while (clck_uptime_ms() - start < 4000) {
        if (kb_haschar()) {
            int key = GetChar();
            if (key == 'x' || key == 'X') {
                boot_options_requested = true;
                break;
            }
        } else {
            cpu_idle();
        }
    }

    return 0;
}

int error_screen (const char* code) {
    clear(RED);
    puts(5, 7, BLACK, RED, "==================================");
    puts(5, 9, BLACK, RED, ">> SYSTEM EXCEPTION <<");
    puts(5, 10, BLACK, RED, "The system encountered an error and was forced to stop.");
    puts(5, 11, BLACK, RED, "Message is as followed:");
    if (StrCompare(code, "invalid_code") == 1) {
        puts(5, 12, BLACK, RED, "[ ! ] OS COMPILER LEVEL EXCEPTION >> INVALID CODE PARSED");
    }
    else {
        puts(5, 12, BLACK, RED, "[ ! ] OS UNHANDLED EXCEPTION >> UNKNOWN ERROR");
    }
    // sound driver for error reporting
    beep(0);
    // wait and load the OS again by re-creating the relevant processes
    clck_hang(4000);
    return 0;
}

// main method to compile input line code, returns 0 on success and -1 for invalid code
int compile (const char* code) {
    int result = 0;

    if (code == NULL) {
        error_screen("invalid_code");
        return -1;
    }

    // "s:<text>" prints (says) the text
    if (strncmp(code, "s:", 2) == 0) {
        printf("%s\n", code + 2);
    }
    else if (StrCompare(code, "update_stack") == 1) {
        p_stack_update();
    }
    else if (StrCompare(code, "init_boot") == 1) {
        init_boot();
    }
    else if (StrCompare(code, "splash_screen") == 1) {
        // print the splash screen
        splash_screen();
    }
    else if (StrCompare(code, "print_stack") == 1) {
        // print (dmp) the process stack
        stack_print();
    }
    else if (StrCompare(code, "flash") == 1) {
        flash_pointer(2, 0);
    }
    else if (StrCompare(code, "terminal") == 1) {
        if (cli_active) {
            printf("The terminal is already running.\n");
        } else {
            cli_init(0);
        }
    }
    else if (StrCompare(code, "iso_launch_options") == 1) {
        boot_options_menu();
    }
    else {
        // invalid code
        error_screen("invalid_code");
        result = -1;
    }

    c_lc = c_lc + 1;

    return result;
}

#endif /* REN_COMP_H */

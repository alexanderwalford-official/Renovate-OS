#include "ren_comp.h"
#include <stdint.h>
#include "primary_definitions.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/input/GIO.h"
#include "../drivers/input/IO.h"
#include "../drivers/audio/tone.h"
#include "../sys/sys_clck.h"
#include "../sys/boot.h"
#include "../sys/recovery.h"
#include "../sys/p_man.h"
#include "../sys/terminal/t_int.h"

static int c_lc = 0; // compiler line counter

// pass arguments to compiler
int comp_handler (char* arg) {
    (void)arg;
    return 0;
}

int splash_screen (void) {
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
        if (!kb_haschar()) {
            cpu_idle();
            continue;
        }
        int key = GetChar();
        if (key == 'x' || key == 'X') {
            boot_options_requested = true;
            break;
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
    if (StrCompare(code, "invalid_code")) {
        puts(5, 12, BLACK, RED, "[ ! ] OS COMPILER LEVEL EXCEPTION >> INVALID CODE PARSED");
    } else {
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
    else if (StrCompare(code, "update_stack")) {
        p_stack_update();
    }
    else if (StrCompare(code, "init_boot")) {
        init_boot();
    }
    else if (StrCompare(code, "splash_screen")) {
        splash_screen();
    }
    else if (StrCompare(code, "print_stack")) {
        stack_print();
    }
    else if (StrCompare(code, "flash")) {
        flash_pointer(2, 0);
    }
    else if (StrCompare(code, "terminal")) {
        // the terminal only returns on shut down, a second one would be nested inside the first
        if (cli_active) {
            printf("The terminal is already running.\n");
        } else {
            cli_init(0);
        }
    }
    else if (StrCompare(code, "iso_launch_options")) {
        boot_options_menu();
    }
    else {
        error_screen("invalid_code");
        result = -1;
    }

    c_lc++;
    return result;
}

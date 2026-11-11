/*
    aer_comp.c - the AerLang compiler

    Every process on the process stack (sys/p_man.c) is a single line of AerLang code. When
    the process manager runs a process it passes that line to compile(), which looks it up
    and calls the kernel function it stands for. That is how the whole boot sequence is
    driven: kernel.c only creates processes called "splash_screen", "init_boot" and
    "terminal", and this file is what turns those names into the real thing.

    It also holds two full screen layouts that are not part of any other module yet:
    the splash screen and the error (system exception) screen. Both are drawn with the
    positional puts() from the VGA driver rather than the scrolling console, and both call
    clear() first, which bumps vga_clear_count so the terminal knows to redraw itself when
    one of them is run from the RUN command.

    The screen is 80 columns by 25 rows, so the column numbers below put the text roughly
    in the middle of the screen.
*/

#include "aer_comp.h"
#include <stdint.h>
#include "primary_definitions.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/input/GIO.h"
#include "../drivers/input/IO.h"
#include "../drivers/audio/tone.h"
#include "../sys/sys_clck.h"
#include "../sys/boot.h"
#include "../sys/recovery.h"
#include "../sys/splash.h"
#include "../sys/sound.h"
#include "../sys/f_sys.h"
#include "../lang/aerlang.h"
#include "../sys/p_man.h"
#include "../sys/terminal/t_int.h"

static int c_lc = 0; // compiler line counter (every non NULL line given to compile(), including ones it rejected)

// pass arguments to compiler
// a placeholder for passing options to the compiler later on, for now it ignores arg and returns 0
int comp_handler (char* arg) {
    (void)arg; // unused for now, this stops the "unused parameter" warning
    return 0;
}

// the boot logo, black text on a light grey (BRIGHT) screen
// it also gives the user a 4 second window to press X, which kernel.c checks afterwards to
// decide whether to open the boot options menu. Returns 0
// gotcha: running it again from the terminal ("run splash_screen") still sets the flag, but nothing
// looks at it after boot, so pressing X then does nothing
int splash_screen (void) {
    // the orange AerOS boot screen (sys/splash.c)
    splash_draw();
    // the startup chime plays in the background, so X still works straight away
    sound_startup();

    // wait up to 4 seconds, pressing X opens the boot options
    // throw away anything typed before the logo appeared, so only a key pressed now counts
    ClearInputbuffer();
    uint32_t start = clkuptms();
    // unsigned subtraction still gives the right answer if the millisecond counter wraps around
    while (clkuptms() - start < 4000) {
        if (!kb_haschar()) {
            // nothing pressed yet: wait a little before checking again. sleep_ms also works when
            // the timer is dead (a bare "hlt" would then sleep until a key is pressed)
            sleep_ms(10);
            continue;
        }
        // any other key is read and ignored, the wait carries on
        int key = getk();
        if (key == 'x' || key == 'X') {
            boot_options_requested = true;
            break; // no need to sit out the rest of the 4 seconds
        }
    }

    // with no keyboard nothing ever arrives, so we simply wait the full 4 seconds and carry on
    // then blank the screen and give the palette its normal colours back
    splash_close();
    return 0;
}

// the system exception screen, a black on red "something went wrong" page
// code = what went wrong, "invalid_code" means compile() was given code it does not understand,
// anything else is shown as an unknown error. Returns 0 after holding the screen for 4 seconds
int error_screen (const char* code) {
    clear(RED);
    // a fixed layout, every line starting 5 columns in, from row 7 down to row 12
    puts(5, 7, BLACK, RED, "==================================");
    puts(5, 9, BLACK, RED, ">> SYSTEM EXCEPTION <<");
    puts(5, 10, BLACK, RED, "The system encountered an error and was forced to stop.");
    puts(5, 11, BLACK, RED, "Message is as followed:");
    // the last line says which error it was
    if (seq(code, "invalid_code")) {
        puts(5, 12, BLACK, RED, "[ ! ] OS COMPILER LEVEL EXCEPTION >> INVALID CODE PARSED");
    } else {
        puts(5, 12, BLACK, RED, "[ ! ] OS UNHANDLED EXCEPTION >> UNKNOWN ERROR");
    }
    // sound driver for error reporting
    // (0 = the default beep, see tone.h)
    beep(0);
    // wait and load the OS again by re-creating the relevant processes
    // in practice this returns to whoever called it after the wait: from the terminal's RUN command,
    // the terminal notices the screen was cleared and redraws itself (see HandleInput in t_int.c)
    clck_hang(4000);
    return 0;
}

// true if the first word of the code names an AerLang program file ("hello.aer", "/bin/x.aer ...")
static bool is_program (const char* code) {
    // the first word ends at the first space, or at the end of the line if there is none
    const char* end = strchr(code, ' ');
    size_t length = end ? (size_t)(end - code) : slen(code);
    return aer_is_program_name(code, length);
}

// main method to compile input line code, returns 0 on success and -1 for invalid code
// (for a program file: -1 if it wasn't found or stopped with an error, see aer_run_file)
// code = one line of AerLang, matched exactly and case sensitively (only "s:" is a prefix)
//
// the instructions AerLang knows:
//   s:<text>            print <text> on its own line, e.g. "s:hello" prints hello
//   update_stack        hand out fresh tickets on the process stack (p_stack_update)
//   init_boot           the boot log and file system start up (sys/boot.c)
//   splash_screen       the boot logo and X for boot options window
//   print_stack         print the process stack table
//   flash               flash a block at the cursor twice (about 2 seconds)
//   terminal            start the terminal, unless it is already running
//   iso_launch_options  the boot options menu (sys/recovery.c)
//   <name>.aer [args]   run an AerLang program file, with the rest of the line as its arguments
// anything else shows the error screen and returns -1
int compile (const char* code) {
    int result = 0;

    // no code at all counts as invalid code
    if (code == NULL) {
        error_screen("invalid_code");
        return -1;
    }

    // "s:<text>" prints (says) the text
    // everything after the two character prefix is printed as it is, spaces and all
    if (strncmp(code, "s:", 2) == 0) {
        prtf("%s\n", code + 2);
    }
    // the fixed instructions, each matched against the whole line
    else if (seq(code, "update_stack")) {
        p_stack_update();
    }
    else if (seq(code, "init_boot")) {
        init_boot();
    }
    else if (seq(code, "splash_screen")) {
        splash_screen();
    }
    else if (seq(code, "print_stack")) {
        stack_print();
    }
    else if (seq(code, "flash")) {
        // 2 flashes, none done yet
        flash_pointer(2, 0);
    }
    else if (seq(code, "terminal")) {
        // the terminal only returns on shut down, a second one would be nested inside the first
        // (the process manager already refuses to re-run the RUNNING terminal process, this catches
        // a brand new "terminal" process, e.g. typing "run terminal" in the terminal)
        if (cli_active) {
            prtf("The terminal is already running.\n");
        } else {
            // 1 = log in first (only asked for in DISK mode, see sys/accounts/auth.h)
            cli_init(1);
        }
    }
    else if (seq(code, "iso_launch_options")) {
        // gotcha: picking an option after boot only changes boot_option, the file system
        // that init_boot already set up stays as it is
        boot_options_menu();
    }
    // checked after the fixed instructions, which never end in ".aer" anyway
    else if (is_program(code)) {
        // "<program>.aer [args]": run an AerLang program file, the rest of the line is its args
        // (".ren" files from before the language was renamed count too, see aer_is_program_name)
        char program[FS_PATH_MAX];
        // copy the first word out on its own, since aer_run_file wants the path as a string
        // ending in '\0', and the code itself carries on into the arguments
        const char* space = strchr(code, ' ');
        size_t length = space ? (size_t)(space - code) : slen(code);
        // a path too long for the buffer is cut short (leaving room for the '\0'), it then
        // simply won't be found
        if (length >= sizeof(program)) {
            length = sizeof(program) - 1;
        }
        memcpy(program, code, length);
        program[length] = '\0';
        // everything after the first space is handed over as the arguments, "" if there are none
        result = aer_run_file(program, space ? space + 1 : "");
    }
    else {
        // unknown code: show the exception screen (this also beeps and waits 4 seconds)
        error_screen("invalid_code");
        result = -1;
    }

    // count every line that got this far, whether it worked or not (a NULL line returned above)
    c_lc++;
    return result;
}

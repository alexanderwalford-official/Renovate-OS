/*
    bi_system.c
    AerLang's built-in functions for the system: waiting (sleep), the time, random numbers,
    what AerOS knows about the computer (sysinfo), queueing programs for GO (queue), running a
    terminal command from the prompt (cmd), what app mode did (appmode) and ending the program
    (exit).

    Which built-ins need which part of AerOS, for app mode, is listed next to aer_profile in
    aer_builtins.c.

    How a built-in works, and how to add one, is at the top of builtins.h.
*/

#include "builtins.h"
#include "../lib/aer_string.h"
#include "../lib/aer_stdlib.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/input/IO.h"
#include "../sys/sys_clck.h"
#include "../sys/sysinfo.h"
#include "../sys/p_man.h"
#include "../sys/terminal/t_int.h"
#include "../sys/appmode.h"

// sleep(ms): wait that many milliseconds (Ctrl+C still stops the program)
value_t bi_sleep (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t ms = int_arg(argv, 0, line, "sleep");
    // sleep in small steps so ctrl + C still gets noticed
    // (20ms at a time, or whatever is left if that is less; ms may end up below 0, which is fine)
    while (ms > 0 && !kb_take_break()) {
        sleep_ms(ms > 20 ? 20 : (uint32_t)ms);
        ms -= 20;
    }
    // time still left means the loop stopped early, so Ctrl+C was pressed
    if (ms > 0) {
        aer_stop(line);
    }
    return aer_nil();
}

// time(): how many milliseconds AerOS has been running, for timing things
value_t bi_time (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    // a whole number only goes up to 2147483647, so after about 24.8 days running this turns negative
    return aer_int((int32_t)clkuptms());
}

// random(n): a random whole number from 0 to n - 1
value_t bi_random (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t range = int_arg(argv, 0, line, "random");
    if (range <= 0) {
        aer_error(line, "random() needs a number above 0");
    }
    // the remainder after dividing by range is always 0 to range - 1. rand() is seeded from
    // the clock in aer_define_constants, so each run gets different numbers
    return aer_int(rand() % range);
}

// add one row of system information to the list as [group, name, value]
// sysinfo_collect calls this once for every row, ctx is the list that sysinfo() made
static void info_row (const char* group, const char* name, const char* value, void* ctx) {
    value_t row = aer_list(3);
    aer_list_push(row.as.obj, aer_string(group, (uint32_t)slen(group)));
    aer_list_push(row.as.obj, aer_string(name, (uint32_t)slen(name)));
    aer_list_push(row.as.obj, aer_string(value, (uint32_t)slen(value)));
    // the push takes over our reference to the row, so there is nothing to release
    aer_list_push((aer_object_t*)ctx, row);
}

// sysinfo(): everything AerOS knows about the computer, as a list of [group, name, value] (see sys/sysinfo.h)
value_t bi_system_info (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    // 64 is a first guess at how many rows there are, the list grows if there are more
    value_t rows = aer_list(64);
    sysinfo_collect(info_row, rows.as.obj);
    return rows;
}

// queue("hello.aer a b"): put a program (and its arguments) at the end of the queue the GO
// command runs, or "s:text" to print a line between programs. Returns how many are now queued,
// or -1 if the process stack is full. The line isn't checked until it runs
value_t bi_queue (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* code = str_arg(argv, 0, line, "queue");
    if (code->length == 0) {
        aer_error(line, "queue() needs a program to queue");
    }
    // p_queue_add copies the line, so the string can go when the program ends
    return aer_int(p_queue_add(code->data.chars));
}

// the most cmd() keeps of what a command prints (the rest is dropped)
#define CMD_OUTPUT_MAX (16 * 1024)

// cmd(line): run a terminal command as if it had been typed at the prompt, and give back what it
// printed as a list of lines, instead of showing it: cmd("la"), len(cmd("mem")).
// Only at the prompt, not in a program: programs have to ask before they touch your files,
// and a terminal command wouldn't. The command can't use the keyboard while it runs, so a
// question it asks gets no answer (DEL's "Delete it? (y/n)" is a no)
value_t bi_cmd (int argc, value_t* argv, int line) {
    (void)argc;
    // aer->shell is only true for the code typed at the terminal's prompt
    if (!aer->shell) {
        aer_error(line, "cmd() only works at the prompt, not in a program");
    }
    // there is only one capture going at a time, so a command that itself ran cmd() would spoil the first
    if (console_capturing()) {
        aer_error(line, "cmd() can't be used inside another cmd()");
    }
    const char* command = str_arg(argv, 0, line, "cmd")->data.chars;
    // run the command with everything it prints going into output instead of onto the screen
    char* output = aer_alloc(CMD_OUTPUT_MAX);
    cli_capture(command, output, CMD_OUTPUT_MAX);

    // one string per line ('\r' and the '\n' left off), with no empty line after the last one
    value_t lines = aer_list(8);
    const char* p = output;
    while (*p != '\0') {
        // strcspn: how many characters there are before the next '\n' (or the end)
        size_t length = strcspn(p, "\n");
        size_t text_length = length > 0 && p[length - 1] == '\r' ? length - 1 : length;
        aer_list_push(lines.as.obj, aer_string(p, (uint32_t)text_length));
        p += length;
        // step over the '\n' itself. Text that ends with one stops the loop here, which is why
        // there is no empty line at the end
        if (*p == '\n') {
            p++;
        }
    }
    aer_free(output);
    return lines;
}

// appmode(): what app mode did for this program, as [the parts of AerOS it uses, KB of memory
// given back for it, KB free], e.g. [["graphics", "files"], 84, 3900]. It also puts away again
// any part the program doesn't use that has started since. At the prompt, which might use
// anything later, it puts away everything it can (a part starts again by itself when needed)
value_t bi_appmode (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    // at the prompt: start again saying it uses nothing (0), so everything that can be is put away.
    // In a program: keep what it was found to use, and put the rest away again
    if (aer->shell) {
        appmode_begin(0);
    } else {
        appmode_again();
    }
    const appmode_t* mode = appmode_now();

    // the names of the parts in use: every part is one bit (1, 2, 4...), so walk the bits and
    // test each one against the set
    value_t used = aer_list(APP_PARTS);
    for (int bit = 0; bit < APP_PARTS; bit++) {
        uint32_t part = 1u << bit;
        if (mode->uses & part) {
            const char* name = appmode_part_name(part);
            aer_list_push(used.as.obj, aer_string(name, (uint32_t)slen(name)));
        }
    }
    // [used, freed, free], the memory figures turned from bytes into KB
    value_t result = aer_list(3);
    aer_list_push(result.as.obj, used);
    aer_list_push(result.as.obj, aer_int((int32_t)(mode->freed / 1024)));
    aer_list_push(result.as.obj, aer_int((int32_t)(mode->free / 1024)));
    return result;
}

// exit(): end the program here, quietly
value_t bi_exit (int argc, value_t* argv, int line) {
    (void)argc; (void)argv;
    // leaving by the error route, but quietly: there is nothing wrong
    (void)line;
    // run_program() in aer_interp.c sees the 2 (an error would be 1) and ends without a message.
    // Like an error, this jumps straight out, so the function never returns a value
    longjmp(aer->error, 2);
}

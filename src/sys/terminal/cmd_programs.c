/*
    cmd_programs.c - the terminal's commands for running programs

        AER                  the interactive AerLang prompt
        RUN                  run a line of AerLang code as a process of its own
        PM                   the process stack (the process manager, sys/p_man.c)
        QUE, GO              line programs up, then run them one after another

    Also run_program(), which t_int.c uses when the word typed isn't a command: "hello" runs
    the app /bin/hello.aer. Every program runs as a process: it is put on the process stack,
    run, and taken off again. Each command is a row in the table in t_int.c, and commands.h
    says how the files of the terminal fit together.
*/

#include "commands.h"
#include <stdint.h>
#include <stddef.h>
#include "../../lib/primary_definitions.h"
#include "../../drivers/video/VGA_linear.h"
#include "../f_sys.h"
#include "../p_man.h"
#include "../explain.h"
#include "../../lang/aerlang.h"


// =================================================================================================
//  Apps and AerLang
// =================================================================================================

// AER: the interactive AerLang prompt
void cli_aer (char* arguments) {
    (void)arguments;
    aer_repl();
}

// run some AerLang code as a process of its own, then remove the process again.
// Returns false if the process stack is full
static bool run_as_process (const char* code) {
    int pid = p_create(code);
    if (pid < 0) {
        prtf("The process stack is full.\n");
        return false;
    }
    // The terminal is itself a RUNNING process, so p_exec() runs just this new one (it knows
    // not to start the terminal a second time)
    p_exec(pid);
    p_destroy(pid);
    return true;
}

// RUN <CODE>: run a line of AerLang code as a new process, e.g. "run s:hello".
// Code that doesn't make sense shows the red error screen, and the terminal is redrawn after
void cli_run (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage("e.g. print_stack or s:hi");
        return;
    }
    run_as_process(arguments);
}

// PM: print the process stack (the terminal itself is in it, RUNNING)
void cli_pman (char* arguments) {
    (void)arguments;
    prtf("> Processes:\n");
    stack_print();
}

// is there a program file with this name, in the current folder or (for a plain name) in one
// of the app folders, /bin/ and /bin/games/ (aer_app_folders, lang/aerlang.h)?
static bool program_exists (const char* file) {
    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (!fs_split_path(file, folder, sizeof(folder), name, sizeof(name))) {
        return false;
    }
    // the current folder (or wherever a path points) comes first...
    if (fs_file_exists(folder, name)) {
        return true;
    }
    // ...then the app folders, in order, but only for a plain name: a path says exactly
    // where to look already
    bool plain_name = strchr(file, '/') == NULL; // "hello", not "games/hello"
    for (int f = 0; plain_name && aer_app_folders[f] != NULL; f++) {
        if (fs_file_exists(aer_app_folders[f], name)) {
            return true;
        }
    }
    return false;
}

// turn a program name and its arguments into the line its process runs:
//     "hello" + "a b"   ->   "hello.aer a b"
// (".aer" is only added if the name doesn't already end in it). False if there is no such program
static bool make_program_line (const char* program, const char* arguments, char* line, size_t size) {
    scpy(line, program, size);
    if (!aer_is_program_name(line, slen(line))) {
        scat(line, ".aer", size);
    }
    if (!program_exists(line)) {
        return false;
    }
    if (arguments[0] != '\0') {
        scat(line, " ", size);
        scat(line, arguments, size);
    }
    return true;
}

// a word that isn't a command may be an app: "hello" runs /bin/hello.aer.
// Returns false if there is no program by that name
bool run_program (const char* program, const char* arguments) {
    char line[P_INST_MAX];
    if (!make_program_line(program, arguments, line, sizeof(line))) {
        return false;
    }
    run_as_process(line);
    return true;
}


// =================================================================================================
//  The queue: programs lined up to run one after another (see "The queue" in sys/p_man.h)
// =================================================================================================

// p_queue_each() calls this for each queued program, in the order they will run
static void print_queued_program (int position, const char* code, void* context) {
    (void)context;
    prtf("  %d. %s\n", position, code);
}

// QUE on its own: list what is queued
static void list_queue (void) {
    if (p_queue_count() == 0) {
        prtf("The queue is empty. %s <program> adds one, %s runs them in order.\n",
             command_name(cli_queue), command_name(cli_go));
        return;
    }
    prtf("Queued, in the order GO will run them:\n");
    p_queue_each(print_queued_program, NULL);
}

// QUE [PROGRAM]:
//     QUE hello a b       put hello.aer (with arguments "a b") at the end of the queue
//     QUE s:some text     queue a line of text to print, handy between programs
//     QUE                 list the queue
//     QUE CLEAR           empty it
void cli_queue (char* arguments) {
    if (arguments[0] == '\0') {
        list_queue();
        return;
    }
    if (strcasecmp(arguments, "clear") == 0) {
        prtf("Took %d program(s) out of the queue.\n", p_queue_clear());
        return;
    }

    // the line the queued process will run: "s:..." is queued as it is, anything else has to
    // be a program that exists now, so a typing mistake is caught here rather than during GO
    char line[P_INST_MAX];
    bool is_message = strncmp(arguments, "s:", 2) == 0;
    if (is_message) {
        scpy(line, arguments, sizeof(line));
    } else {
        char* program_arguments;
        char* program = split_first_word(arguments, &program_arguments);
        if (!make_program_line(program, program_arguments, line, sizeof(line))) {
            explain_say(PROBLEM_NOT_FOUND, program, "There is no program called %s to queue.", program);
            return;
        }
    }

    // queued programs wait on the process stack, so a full stack means no room in the queue
    int queued = p_queue_add(line);
    if (queued < 0) {
        prtf("The process stack is full.\n");
    } else {
        prtf("Queued %s (%d in the queue, %s runs them).\n", line, queued, command_name(cli_go));
    }
}

// GO calls this before each queued program: a grey "[2/3] hello.aer" line
static void announce_queued_program (int number, int total, const char* code) {
    colour(DARK_GRAY, BLACK);
    prtf("[%d/%d] %s\n", number, total, code);
    colour(BRIGHT, BLACK);
}

// GO calls this after each queued program: Ctrl+C in one of them stops the rest too
static bool queue_should_stop (void) {
    return aer_take_stopped();
}

// GO: run everything in the queue, in the order it was queued
void cli_go (char* arguments) {
    (void)arguments;
    if (p_queue_count() == 0) {
        prtf("The queue is empty. %s <program> adds one.\n", command_name(cli_queue));
        return;
    }

    aer_take_stopped(); // forget a Ctrl+C from before GO started
    // p_queue_run returns how many it ran. If it stopped early, the ones it didn't get to are
    // still queued, so another GO carries on from there
    int ran = p_queue_run(announce_queued_program, queue_should_stop);
    int left = p_queue_count();

    if (left > 0) {
        prtf("Stopped the queue after %d program(s), %d still queued (%s carries on).\n",
             ran, left, command_name(cli_go));
    } else {
        prtf("Ran %d program(s), the queue is empty.\n", ran);
    }
}

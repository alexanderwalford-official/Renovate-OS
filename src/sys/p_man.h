#ifndef P_MAN_H
#define P_MAN_H

#include <stdbool.h>

/*
    p_man.h - the AerOS process manager

    A "process" in AerOS is not a separate program with its own memory and registers
    like on Linux or Windows. It is a line of AerLang code (e.g. "splash_screen", "terminal",
    "s:hello") sitting in a slot on the process stack, waiting to be handed to the compiler
    (lib/aer_comp.c), which runs it. Everything is single threaded and cooperative: a process
    runs from start to finish before the next one gets a go, and there is no timer driven
    switching between processes yet.

    The life of a process:
        p_create()   -> a free slot is filled in, the process is DEAD (it exists but is not queued)
        p_exec()     -> it becomes LIVE, tickets are handed out and every LIVE process is run
        (running)    -> RUNNING while the compiler executes its code
        (finished)   -> DEAD again, with no execution time left
        p_destroy()  -> the slot is wiped so its ID can be handed out again

    Things To Note:
    - Once a process is created, it does not execute immediately. This must be called using: p_exec(p_id).
    - p_create() returns the new process ID (or -1 if the process stack is full), destroyed IDs are re-used.
    - Process execution time is calculated based on the length of the input string, each character counting for 0.01 seconds (stored in ms).
    - The process stack is automatically updated (tickets handed out, shortest execution time first) when a process is executed.
    - All processes will be executed after execution of an initial process if they are live.
    - A process that has finished executing becomes dead, it stays on the stack until p_destroy().
    - A process that is RUNNING is never started again by a nested p_exec(). The terminal is the
      classic example: it stays RUNNING for as long as the OS is up, and every RUN command inside
      it calls p_exec() again, so without this rule the terminal would keep restarting itself.

    Scheduling in a nutshell ("shortest job first"):
    The execution time is only an estimate (longer code = longer job). p_stack_update() sorts the
    LIVE processes by it and gives them tickets 1, 2, 3 ... and p_stack_runtime() always runs the
    lowest ticket next. https://en.wikipedia.org/wiki/Shortest_job_next

    The queue:
    AerOS runs one thing at a time, so instead of running programs side by side they can be lined
    up to run one after another. p_queue_add() puts a process on the stack as WAITING, a state the
    scheduler above leaves alone (a LIVE process would be run by the next p_exec() of anything at
    all). p_queue_run() then runs the WAITING ones in the order they were added, each through
    p_exec(), and destroys each when it is done:

        QUE hello        ->  [ hello.aer  WAITING #1 ]
        QUE guess        ->  [ hello.aer  WAITING #1 ] [ guess.aer  WAITING #2 ]
        GO               ->  hello.aer runs to the end, then guess.aer
*/

// how many processes can exist at once (the number of slots on the process stack)
#define P_STACK_SIZE 256
// the longest line of code a process can hold, including the terminating '\0' (longer code is cut short)
#define P_INST_MAX   128

// process run states
#define P_DEAD    0 // not scheduled (finished, or never executed)
#define P_LIVE    1 // queued for execution
#define P_FROZEN  2 // paused by p_freeze()
#define P_RUNNING 3 // currently executing
#define P_WAITING 4 // in the queue (p_queue_add), runs when p_queue_run gets to it

// one slot of the process stack
typedef struct process {
    unsigned int p_id; // process ID (always the same as the slot's index in p_stack)
    unsigned int p_et; // process execution time (ms), 10ms per character of code, 0 once it has run
    signed int p_rs; // process runstate, see P_DEAD / P_LIVE / P_FROZEN / P_RUNNING
    unsigned int p_t; // process ticket - when it will be executed, defined by the process stack (0 = none)
    char p_inst[P_INST_MAX]; // process instructions (the AerLang code handed to compile())
    bool p_used; // this slot of the process stack holds a process
    unsigned int p_seq; // WAITING processes: their place in the queue (lower runs first)
} process_t;

extern int p_cnt; // process count
extern struct process p_stack[P_STACK_SIZE]; // every slot, used or not (check p_used)

// run a process action by letter: "d" destroy, "f" freeze for 1ms, "e" execute, "p" print the stack
// returns 0 if the action was known, -1 if not
int p_id_handler (const char* action, int p_id);

// make a new (DEAD) process holding the code c_p_inst, returns its ID or -1 if the stack is full / c_p_inst is NULL
int p_create (const char* c_p_inst);

// wipe a process off the stack so its ID can be re-used (invalid IDs are ignored)
void p_destroy (int p_id);

// mark a process FROZEN and hang the whole OS for t milliseconds, then restore its old state
void p_freeze (int p_id, unsigned int t);

// queue a process (LIVE) and run everything that is LIVE, shortest first. Does nothing if it is already RUNNING
void p_exec (int p_id);

// hand out tickets (1 = runs first) to every LIVE process, sorted by execution time
void p_stack_update (void);

// keep running the LIVE process with the lowest ticket until none are left
void p_stack_runtime (void);

// run one process right now: RUNNING while its code is compiled, DEAD afterwards
void p_stack_exec (int p_id);

// print a table of every process on the stack (used by PM and "print_stack")
void stack_print (void);

// add code to the end of the queue (WAITING), returns how many are now queued, -1 if the stack is full
int p_queue_add (const char* code);

// how many processes are waiting in the queue
int p_queue_count (void);

// call fn with each queued process's code, in the order they will run
void p_queue_each (void (*fn)(int position, const char* code, void* ctx), void* ctx);

// take everything out of the queue without running it, returns how many there were
int p_queue_clear (void);

// run the queue, oldest first, until it is empty or stop() (may be NULL) says to stop after a
// process. before (may be NULL) is told which one is next, as "number of total". Returns how many ran
int p_queue_run (void (*before)(int number, int total, const char* code), bool (*stop)(void));

#endif /* P_MAN_H */

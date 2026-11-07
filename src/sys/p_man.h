#ifndef P_MAN_H
#define P_MAN_H

#include <stdint.h>
#include <stdbool.h>
#include "../drivers/video/VGA_linear.h"
#include "../lib/primary_definitions.h"
#include "sys_clck.h"

/*
    Things To Note:
    - Once a process is created, it does not execute immediately. This must be called using: p_exec(p_id).
    - p_create() returns the new process ID (or -1 if the process stack is full), destroyed IDs are re-used.
    - Process execution time is calculated based on the length of the input string, each character counting for 0.01 seconds (stored in ms).
    - The process stack is automatically updated (tickets handed out, shortest execution time first) when a process is executed.
    - All processes will be executed after execution of an initial process if they are live.
    - A process that has finished executing becomes dead, it stays on the stack until p_destroy().
*/

#define P_STACK_SIZE 256
#define P_INST_MAX   128

// process run states
#define P_DEAD    0 // not scheduled (finished, or never executed)
#define P_LIVE    1 // queued for execution
#define P_FROZEN  2 // paused by p_freeze()
#define P_RUNNING 3 // currently executing

// structures
typedef struct process {
    unsigned int p_id; // process ID
    unsigned int p_et; // process execution time (ms)
    signed int p_rs; // process runstate, see P_DEAD / P_LIVE / P_FROZEN / P_RUNNING
    unsigned int p_t; // process ticket - when it will be executed, defined by the process stack (0 = none)
    char p_inst[P_INST_MAX]; // process instructions
    bool p_used; // this slot of the process stack holds a process
} process_t;

// dynamic variables
int p_cnt = 0; // process count, automatic handler
struct process p_stack[P_STACK_SIZE]; // process stack

// prototypes (p_man.h and ren_comp.h use each other)
int p_id_handler (const char* action, int p_id);
int p_create (const char* c_p_inst);
void p_destroy (int p_id);
void p_freeze (int p_id, unsigned int t);
void p_exec (int p_id);
void p_stack_update (void);
void p_stack_runtime (void);
void p_stack_exec (int p_id);
void stack_print (void);

#include "../lib/ren_comp.h"

// true if p_id refers to an existing process
bool p_valid (int p_id) {
    return p_id >= 0 && p_id < P_STACK_SIZE && p_stack[p_id].p_used;
}

// primary process handler (alternate method)
int p_id_handler (const char* action, int p_id) {
    if (StrCompare(action, "d")) p_destroy(p_id);
    else if (StrCompare(action, "f")) p_freeze(p_id, 1);
    else if (StrCompare(action, "e")) p_exec(p_id);
    else if (StrCompare(action, "p")) stack_print();
    else return -1;
    return 0;
}

// create new process, returns its ID or -1 when the process stack is full
int p_create (const char* c_p_inst) {
    if (c_p_inst == NULL) {
        return -1;
    }
    for (int id = 0; id < P_STACK_SIZE; ++id) {
        if (!p_stack[id].p_used) {
            struct process* p = &p_stack[id];
            memset(p, 0, sizeof(*p));
            p->p_used = true;
            p->p_id = (unsigned int)id; // ID - the first free slot
            strlcpy(p->p_inst, c_p_inst, sizeof(p->p_inst)); // instructions
            p->p_et = (unsigned int)strlen(p->p_inst) * 10; // execution time, 0.01 seconds per character
            p->p_rs = P_DEAD;
            p->p_t = 0;
            p_cnt = p_cnt + 1; // increase process count
            return id;
        }
    }
    return -1;
}

// destroy a process, its ID can then be re-used
void p_destroy (int p_id) {
    if (!p_valid(p_id)) {
        return;
    }
    memset(&p_stack[p_id], 0, sizeof(p_stack[p_id])); // remove all process data
    p_cnt = p_cnt - 1; // decrease process count
}

// freeze a process (milliseconds)
void p_freeze (int p_id, unsigned int t) {

    /*
        Things to note:
        Currently, the objective is to make the operating system 'hang' when this is called.
        Once multi-threading has been implemented, this would have to be changed.

        More features such as invoke event handlers will be added here eventually.
    */

    bool valid = p_valid(p_id);
    int previous = valid ? p_stack[p_id].p_rs : P_DEAD;

    // mark the process as frozen
    if (valid) {
        p_stack[p_id].p_rs = P_FROZEN;
    }

    // hang the system for the requested time
    clck_hang((long)t);

    // restore the previous state
    if (valid && p_stack[p_id].p_used) {
        p_stack[p_id].p_rs = previous;
    }
}

// repetative process execution (compilation)
void p_exec (int p_id) {
    if (!p_valid(p_id) || p_stack[p_id].p_rs == P_RUNNING) {
        return;
    }

    // first, add to the process stack so that it can be queued for execution by setting its state to live
    p_stack[p_id].p_rs = P_LIVE;

    // update the process stack (hand out tickets)
    p_stack_update();

    // execute all live processes
    p_stack_runtime();
}

// sort helper: shortest execution time first, then lowest ID
int p_compare_et (const void* a, const void* b) {
    const struct process* pa = &p_stack[*(const int*)a];
    const struct process* pb = &p_stack[*(const int*)b];
    if (pa->p_et != pb->p_et) {
        return (pa->p_et < pb->p_et) ? -1 : 1;
    }
    return (pa->p_id < pb->p_id) ? -1 : (pa->p_id > pb->p_id);
}

// update method for the process stack
// redefine the execution schedule by updating processes ticket ID if p_rs is live
// orders the live processes by execution time, ticket 1 runs first
void p_stack_update (void) {
    int order[P_STACK_SIZE];
    int count = 0;

    for (int i = 0; i < P_STACK_SIZE; ++i) {
        p_stack[i].p_t = 0;
        if (p_stack[i].p_used && p_stack[i].p_rs == P_LIVE) {
            order[count++] = i;
        }
    }

    qsort(order, (size_t)count, sizeof(order[0]), p_compare_et);

    for (int i = 0; i < count; ++i) {
        p_stack[order[i]].p_t = (unsigned int)(i + 1);
    }
}

// calls each live process in ticket order
void p_stack_runtime (void) {
    for (;;) {
        int next = -1;
        for (int i = 0; i < P_STACK_SIZE; ++i) {
            if (p_stack[i].p_used && p_stack[i].p_rs == P_LIVE &&
                (next < 0 || p_stack[i].p_t < p_stack[next].p_t)) {
                next = i;
            }
        }
        if (next < 0) {
            break; // nothing left to run
        }
        p_stack_exec(next);
    }
}

// executes the code in the process
void p_stack_exec (int p_id) {
    if (!p_valid(p_id)) {
        return;
    }
    struct process* p = &p_stack[p_id];
    p->p_rs = P_RUNNING;

    // handle execution
    if (p->p_inst[0] == '\0') {
        // no instructions, print it
        printf("INSTRUCTIONS NULL\n");
    }
    else {
        compile(p->p_inst);
    }

    // once executed there is no execution time left, the process is finished
    if (p->p_used && p->p_rs == P_RUNNING) {
        p->p_rs = P_DEAD;
        p->p_et = 0;
        p->p_t = 0;
    }
}

const char* p_state_name (int state) {
    switch (state) {
        case P_DEAD: return "DEAD";
        case P_LIVE: return "LIVE";
        case P_FROZEN: return "FROZEN";
        case P_RUNNING: return "RUNNING";
        default: return "?";
    }
}

// print the process stack
void stack_print (void) {
    printf("Process Stack: %d process(es)\n", p_cnt);
    printf("  PID  STATE    ET(ms)  TICKET  INSTRUCTIONS\n");
    for (int i = 0; i < P_STACK_SIZE; ++i) {
        if (p_stack[i].p_used) {
            const struct process* p = &p_stack[i];
            printf("  %3u  %-7s  %6u  %6u  %s\n", p->p_id, p_state_name(p->p_rs), p->p_et, p->p_t, p->p_inst);
        }
    }
}

#endif /* P_MAN_H */

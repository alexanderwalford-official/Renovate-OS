/*
    p_man.c - the process manager

    Keeps the process stack (a fixed array of P_STACK_SIZE slots), creates and destroys
    processes, decides in which order LIVE processes run and runs them by handing their
    code to the AerLang compiler (lib/aer_comp.c).

    Despite the name, the "process stack" is not a stack in the push / pop sense. It is a
    table of slots indexed by process ID, the scheduler walks it to find the next job.

    There is no preemption: a process runs until its code has finished. If that code starts
    another process (for example the RUN command in the terminal), p_exec() is called again
    from inside the first one, so the scheduler can be "nested" several calls deep. The run
    states (see p_man.h) are what keep that safe.
*/

#include "p_man.h"
#include "../lib/primary_definitions.h"
#include "../lib/aer_comp.h"
#include "sys_clck.h"

int p_cnt = 0; // process count, automatic handler
struct process p_stack[P_STACK_SIZE]; // process stack
static unsigned int p_next_seq = 1; // the queue position the next p_queue_add() hands out

// true if p_id refers to an existing process
// (in range for the array, and the slot is actually in use rather than empty)
static bool p_valid (int p_id) {
    return p_id >= 0 && p_id < P_STACK_SIZE && p_stack[p_id].p_used;
}

// primary process handler (alternate method)
// lets a caller pick an action with a single letter instead of calling the functions directly:
//   "d" = p_destroy, "f" = p_freeze for 1ms, "e" = p_exec, "p" = stack_print (p_id is ignored)
// returns 0 if the action was recognised, -1 if not
int p_id_handler (const char* action, int p_id) {
    if (StrCompare(action, "d")) {
        p_destroy(p_id);
    }
    else if (StrCompare(action, "f")) {
        p_freeze(p_id, 1);
    }
    else if (StrCompare(action, "e")) {
        p_exec(p_id);
    }
    else if (StrCompare(action, "p")) {
        stack_print();
    }
    else return -1;
    return 0;
}

// create new process, returns its ID or -1 when the process stack is full
// c_p_inst = the AerLang code the process will run, it is copied so the caller's string can go away
// the process is not run yet, call p_exec() with the returned ID for that
int p_create (const char* c_p_inst) {
    if (c_p_inst == NULL) {
        return -1;
    }
    // look for the lowest free slot, so IDs of destroyed processes are re-used straight away
    for (int id = 0; id < P_STACK_SIZE; ++id) {
        struct process* p = &p_stack[id];
        if (p->p_used) {
            continue;
        }
        // the first free slot, the process waits (dead) until p_exec()
        // wipe it first so nothing from a previous process is left behind
        memset(p, 0, sizeof(*p));
        p->p_used = true;
        p->p_id = (unsigned int)id;
        // strlcpy always '\0' terminates, code longer than P_INST_MAX - 1 characters is cut short
        strlcpy(p->p_inst, c_p_inst, sizeof(p->p_inst));
        // the execution time is measured from the copy, so cut short code gets a matching (shorter) time
        p->p_et = (unsigned int)strlen(p->p_inst) * 10; // 0.01 seconds per character
        p->p_rs = P_DEAD;
        p_cnt++;
        return id;
    }
    // every slot is in use
    return -1;
}

// destroy a process, its ID can then be re-used
// zeroing the slot also clears p_used, which is what marks it as free for p_create()
void p_destroy (int p_id) {
    if (!p_valid(p_id)) {
        return; // already free (or never existed), so the count must not go down twice
    }
    memset(&p_stack[p_id], 0, sizeof(p_stack[p_id]));
    p_cnt--;
}

// freeze a process (milliseconds)
// p_id = the process to mark as frozen, t = how long to freeze for
// gotcha: this blocks everything, not just the one process, because there is only one thread
void p_freeze (int p_id, unsigned int t) {

    /*
        Things to note:
        Currently, the objective is to make the operating system 'hang' when this is called.
        Once multi-threading has been implemented, this would have to be changed.

        More features such as invoke event handlers will be added here eventually.
    */

    // an unknown process still gets the hang, there is just no state to change
    if (!p_valid(p_id)) {
        clck_hang((long)t);
        return;
    }

    // remember the state so a LIVE process goes back to LIVE (or a RUNNING one to RUNNING) afterwards
    signed int previous = p_stack[p_id].p_rs;
    p_stack[p_id].p_rs = P_FROZEN;
    clck_hang((long)t);
    p_stack[p_id].p_rs = previous;
}

// repetative process execution (compilation)
// queue the process and then run every LIVE process (not just this one), shortest first
// a finished (DEAD) process keeps its code until p_destroy(), so calling this again runs it again
void p_exec (int p_id) {
    // a RUNNING process is already somewhere further up the call stack (e.g. the terminal while it
    // runs a RUN command), starting it again would run it inside itself, so leave it alone
    if (!p_valid(p_id) || p_stack[p_id].p_rs == P_RUNNING) {
        return;
    }

    // first, add to the process stack so that it can be queued for execution by setting its state to live
    p_stack[p_id].p_rs = P_LIVE;

    // hand out tickets, then run everything that is live
    p_stack_update();
    p_stack_runtime();
}

// sort helper: shortest execution time first, then lowest ID
// qsort hands us pointers to two entries of the order[] array in p_stack_update(), and each entry
// is a slot index, so look the processes up in p_stack. Returns <0, 0 or >0 like strcmp.
// the ID tie-break makes the order predictable when two processes have the same length of code
static int p_compare_et (const void* a, const void* b) {
    const struct process* pa = &p_stack[*(const int*)a];
    const struct process* pb = &p_stack[*(const int*)b];
    if (pa->p_et != pb->p_et) {
        return (pa->p_et < pb->p_et) ? -1 : 1;
    }
    // -1 if a has the lower ID, 1 if b does, 0 only if they are the same process
    return (pa->p_id < pb->p_id) ? -1 : (pa->p_id > pb->p_id);
}

// update method for the process stack
// redefine the execution schedule by giving every live process a ticket, ticket 1 runs first
void p_stack_update (void) {
    int order[P_STACK_SIZE]; // slot indexes of the LIVE processes, sorted below
    int count = 0;

    // every ticket from the last schedule is thrown away (DEAD, FROZEN and RUNNING processes get 0)
    // and the LIVE processes are collected so they can be sorted
    for (int i = 0; i < P_STACK_SIZE; ++i) {
        p_stack[i].p_t = 0;
        if (p_stack[i].p_used && p_stack[i].p_rs == P_LIVE) {
            order[count++] = i;
        }
    }

    // shortest execution time first (see p_compare_et)
    qsort(order, (size_t)count, sizeof(order[0]), p_compare_et);

    // tickets count from 1, so 0 can mean "no ticket"
    for (int i = 0; i < count; ++i) {
        p_stack[order[i]].p_t = (unsigned int)(i + 1);
    }
}

// calls each live process in ticket order
void p_stack_runtime (void) {
    // running a process can change the stack (it may run or queue others), so look for the next ticket every time
    // (a nested p_exec() inside the process may already have run some of the LIVE ones, re-scanning
    // means we never run something twice or miss something new)
    for (;;) {
        // find the LIVE process with the lowest ticket
        int next = -1;
        for (int i = 0; i < P_STACK_SIZE; ++i) {
            if (p_stack[i].p_used && p_stack[i].p_rs == P_LIVE &&
                (next < 0 || p_stack[i].p_t < p_stack[next].p_t)) {
                next = i;
            }
        }
        // nothing is LIVE any more, we are done
        if (next < 0) {
            return;
        }
        // p_stack_exec() always leaves the process DEAD, so this loop is guaranteed to move on
        p_stack_exec(next);
    }
}

// executes the code in the process
// the process is RUNNING for as long as compile() takes, which for the terminal is forever
void p_stack_exec (int p_id) {
    if (!p_valid(p_id)) {
        return;
    }
    struct process* p = &p_stack[p_id];
    p->p_rs = P_RUNNING;

    // a process with no code has nothing to compile (p_create("") makes one of these)
    if (p->p_inst[0] == '\0') {
        prtf("INSTRUCTIONS NULL\n");
    } else {
        compile(p->p_inst);
    }

    // once executed there is no execution time left, the process is finished
    // (it stays on the stack as DEAD until whoever created it calls p_destroy())
    p->p_rs = P_DEAD;
    p->p_et = 0;
    p->p_t = 0;
}

// a run state as a word for the stack table, "?" for anything unexpected
static const char* p_state_name (int state) {
    switch (state) {
        case P_DEAD: return "DEAD";
        case P_LIVE: return "LIVE";
        case P_FROZEN: return "FROZEN";
        case P_RUNNING: return "RUNNING";
        case P_WAITING: return "WAITING";
        default: return "?";
    }
}

// print the process stack
// one row per used slot: ID, run state, execution time left, ticket and the code itself
// e.g. from the terminal, PMAN shows the terminal process as RUNNING, and "run print_stack" shows
// both the terminal and the print_stack process, each RUNNING
void stack_print (void) {
    prtf("Process Stack: %d process(es)\n", p_cnt);
    prtf("  PID  STATE    ET(ms)  TICKET  INSTRUCTIONS\n");
    for (int i = 0; i < P_STACK_SIZE; ++i) {
        const struct process* p = &p_stack[i];
        if (p->p_used) {
            // the field widths line the columns up under the heading above
            prtf("  %3u  %-7s  %6u  %6u  %s\n", p->p_id, p_state_name(p->p_rs), p->p_et, p->p_t, p->p_inst);
        }
    }
}

// ---- the queue ----------------------------------------------------------------------------------
// see "The queue" in p_man.h

int p_queue_add (const char* code) {
    int id = p_create(code);
    if (id < 0) {
        return -1;
    }
    p_stack[id].p_rs = P_WAITING;
    p_stack[id].p_seq = p_next_seq++;
    return p_queue_count();
}

int p_queue_count (void) {
    int count = 0;
    for (int i = 0; i < P_STACK_SIZE; ++i) {
        if (p_stack[i].p_used && p_stack[i].p_rs == P_WAITING) {
            count++;
        }
    }
    return count;
}

// the WAITING process that was queued first, -1 if the queue is empty
static int p_queue_first (void) {
    int first = -1;
    for (int i = 0; i < P_STACK_SIZE; ++i) {
        if (p_stack[i].p_used && p_stack[i].p_rs == P_WAITING &&
            (first < 0 || p_stack[i].p_seq < p_stack[first].p_seq)) {
            first = i;
        }
    }
    return first;
}

void p_queue_each (void (*fn)(int position, const char* code, void* ctx), void* ctx) {
    // walk the queue in order by repeatedly finding the next sequence number after the last one
    unsigned int after = 0;
    for (int position = 1;; position++) {
        int next = -1;
        for (int i = 0; i < P_STACK_SIZE; ++i) {
            if (p_stack[i].p_used && p_stack[i].p_rs == P_WAITING && p_stack[i].p_seq > after &&
                (next < 0 || p_stack[i].p_seq < p_stack[next].p_seq)) {
                next = i;
            }
        }
        if (next < 0) {
            return;
        }
        after = p_stack[next].p_seq;
        fn(position, p_stack[next].p_inst, ctx);
    }
}

int p_queue_clear (void) {
    int cleared = 0;
    for (int i = 0; i < P_STACK_SIZE; ++i) {
        if (p_stack[i].p_used && p_stack[i].p_rs == P_WAITING) {
            p_destroy(i);
            cleared++;
        }
    }
    return cleared;
}

int p_queue_run (void (*before)(int number, int total, const char* code), bool (*stop)(void)) {
    int ran = 0;
    for (;;) {
        int next = p_queue_first();
        if (next < 0) {
            break;
        }
        ran++;
        // a program that queues more while it runs makes the total grow, so it is counted each time
        if (before) {
            before(ran, ran + p_queue_count() - 1, p_stack[next].p_inst);
        }
        p_exec(next); // WAITING -> LIVE -> RUNNING -> DEAD
        p_destroy(next);
        if (stop && stop()) {
            break;
        }
    }
    return ran;
}

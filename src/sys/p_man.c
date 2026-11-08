#include "p_man.h"
#include "../lib/primary_definitions.h"
#include "../lib/ren_comp.h"
#include "sys_clck.h"

int p_cnt = 0; // process count, automatic handler
struct process p_stack[P_STACK_SIZE]; // process stack

// true if p_id refers to an existing process
static bool p_valid (int p_id) {
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
        struct process* p = &p_stack[id];
        if (p->p_used) {
            continue;
        }
        // the first free slot, the process waits (dead) until p_exec()
        memset(p, 0, sizeof(*p));
        p->p_used = true;
        p->p_id = (unsigned int)id;
        strlcpy(p->p_inst, c_p_inst, sizeof(p->p_inst));
        p->p_et = (unsigned int)strlen(p->p_inst) * 10; // 0.01 seconds per character
        p->p_rs = P_DEAD;
        p_cnt++;
        return id;
    }
    return -1;
}

// destroy a process, its ID can then be re-used
void p_destroy (int p_id) {
    if (!p_valid(p_id)) {
        return;
    }
    memset(&p_stack[p_id], 0, sizeof(p_stack[p_id]));
    p_cnt--;
}

// freeze a process (milliseconds)
void p_freeze (int p_id, unsigned int t) {

    /*
        Things to note:
        Currently, the objective is to make the operating system 'hang' when this is called.
        Once multi-threading has been implemented, this would have to be changed.

        More features such as invoke event handlers will be added here eventually.
    */

    if (!p_valid(p_id)) {
        clck_hang((long)t);
        return;
    }

    signed int previous = p_stack[p_id].p_rs;
    p_stack[p_id].p_rs = P_FROZEN;
    clck_hang((long)t);
    p_stack[p_id].p_rs = previous;
}

// repetative process execution (compilation)
void p_exec (int p_id) {
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
static int p_compare_et (const void* a, const void* b) {
    const struct process* pa = &p_stack[*(const int*)a];
    const struct process* pb = &p_stack[*(const int*)b];
    if (pa->p_et != pb->p_et) {
        return (pa->p_et < pb->p_et) ? -1 : 1;
    }
    return (pa->p_id < pb->p_id) ? -1 : (pa->p_id > pb->p_id);
}

// update method for the process stack
// redefine the execution schedule by giving every live process a ticket, ticket 1 runs first
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
    // running a process can change the stack (it may run or queue others), so look for the next ticket every time
    for (;;) {
        int next = -1;
        for (int i = 0; i < P_STACK_SIZE; ++i) {
            if (p_stack[i].p_used && p_stack[i].p_rs == P_LIVE &&
                (next < 0 || p_stack[i].p_t < p_stack[next].p_t)) {
                next = i;
            }
        }
        if (next < 0) {
            return;
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

    if (p->p_inst[0] == '\0') {
        printf("INSTRUCTIONS NULL\n");
    } else {
        compile(p->p_inst);
    }

    // once executed there is no execution time left, the process is finished
    p->p_rs = P_DEAD;
    p->p_et = 0;
    p->p_t = 0;
}

static const char* p_state_name (int state) {
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
        const struct process* p = &p_stack[i];
        if (p->p_used) {
            printf("  %3u  %-7s  %6u  %6u  %s\n", p->p_id, p_state_name(p->p_rs), p->p_et, p->p_t, p->p_inst);
        }
    }
}

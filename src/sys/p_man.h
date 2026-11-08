#ifndef P_MAN_H
#define P_MAN_H

#include <stdbool.h>

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

typedef struct process {
    unsigned int p_id; // process ID
    unsigned int p_et; // process execution time (ms)
    signed int p_rs; // process runstate, see P_DEAD / P_LIVE / P_FROZEN / P_RUNNING
    unsigned int p_t; // process ticket - when it will be executed, defined by the process stack (0 = none)
    char p_inst[P_INST_MAX]; // process instructions
    bool p_used; // this slot of the process stack holds a process
} process_t;

extern int p_cnt; // process count
extern struct process p_stack[P_STACK_SIZE];

int p_id_handler (const char* action, int p_id);
int p_create (const char* c_p_inst);
void p_destroy (int p_id);
void p_freeze (int p_id, unsigned int t);
void p_exec (int p_id);
void p_stack_update (void);
void p_stack_runtime (void);
void p_stack_exec (int p_id);
void stack_print (void);

#endif /* P_MAN_H */

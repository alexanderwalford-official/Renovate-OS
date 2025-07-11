#ifndef P_MAN_H
#define P_MAN_H

#include "stdint.h"
#include "../drivers/video/VGA_linear.h"
#include "../lib/ren_comp.h"
#include "../lib/primary_definitions.h"
#include "../sys/sys_clck.h"

/*
    Things To Note:
    - Once a process is created, it does not execute immediately. This must be called using: p_exec(p_id, p_et).
    - Process execution time is calculated based on the length of the input string, each character counting for 0.01 seconds.
    - The process stack is automatically updated when the process is executed.
    - All processes will be executed after execution of an initial process is they are live.
*/

// structures
typedef struct process {
    unsigned int p_id; // process ID
    unsigned int p_et; // process execution time
    signed int p_rs; // process runstate, 0 = dead, 1 = live
    unsigned int p_t; // process ticket - when it will be executed, defined by the process stack
    char* p_inst; // process instructions
};

// constants
const float i = 0.0f;

// dynamic variables
int p_cnt = 0; // process count, automatic handler
struct process p_stack[256]; // process stack

// primary process handler (alternate method)
int p_id_handler (char* action, int p_id) {
    (action == "d") ? p_destory(p_id) : (action == "f") ? p_freeze(p_id, 1) : (action == "e") ? p_exec(p_id) : (action == "p") ? stack_print() : ret();
    return 0;
}

// create new process
void p_create (char* c_p_inst) {
    struct process p;
    p.p_id = p_cnt; // ID - should be the count
    p.p_inst = c_p_inst; // instructions
    p.p_et = sizeof(c_p_inst) / sizeof(c_p_inst[0]) / 0.01f; // execution time
    p_stack[p.p_id] = p; // add process to process stack structure
    p_cnt = p_cnt + 1; // increase process count
}

// destory a process
void p_destory (int p_id) {
    p_stack[p_id].p_id = 000; // set the PID to 000, making it possible to re-use it in memory for creating new processes
    p_stack[p_id].p_et = 0; // reset execution time to 0
    p_stack[p_id].p_rs = 0; // would normally make the process dead but not deleted but due to p_id = 000 this can be over-written
    p_stack[p_id].p_inst = NULL; // remove all process instructions
    p_cnt = p_cnt - 1; // decrease process count
}

// freeze a process (milliseconds)
void p_freeze (int p_id, float t) {

    /*
        Things to note:
        Currently, the objective is to make the operating system 'hang' when this is called.
        Once multi-threading has been implemented, this would have to be changed.

        More features such as invoke event handlers will be added here eventually.
    */

    // set the process as dead
    p_stack[p_id].p_rs = 0;

    // hang the clock by making the CPU complete iterative operations
    clck_hang(t, 0);

    // set the process as live again
    p_stack[p_id].p_rs = 1;
}

// repetative process execution (compilation)
void p_exec (int p_id) {   
    // first, add to the process stack so that it can be queued for execution by setting its state to live
    p_stack[p_id].p_rs = 1; // live

    // execute all live processes
    p_stack_runtime();

    // update the process stack
    p_stack_update(0); 
}

// dynamic vars
int p_et_stack[];

// update method for the process stack
// redefine the execution schedule by updating processes ticket ID if p_rs is 1 (live)
// uses a linear sorting algorithm to order processes by execution time

void p_stack_update(int cn) {
    // base case: end of array
    if (cn >= sizeof(p_stack) / sizeof(p_stack[0])) {
        return;
    }

    // skip inactive processes (p_rs == 0)
    if (p_stack[cn].p_rs == 0) {
        // move to the next process
        p_stack_update(cn + 1);
        return;
    }

    // store p_et for comparison
    unsigned int current_p_et = p_stack[cn].p_et;

    // if not the first element, compare with the previous one
    if (cn > 0) {
        // compare with the previous process's p_et
        if (current_p_et < p_stack[cn - 1].p_et) {
            // swap p_et values in p_stack
            unsigned int temp = p_stack[cn].p_et;
            p_stack[cn].p_et = p_stack[cn - 1].p_et;
            p_stack[cn - 1].p_et = temp;
        }
    }

    // move to the next process
    p_stack_update(cn + 1);
}

// calls each process in order of execution by looping through process stack elements
void p_stack_runtime () {
    int size = sizeof(p_stack) / sizeof(p_stack[0]); // get the size of the process stack
    for (int i = 0; i < size; ++i) {
        //printf("Process ID: %d, Execution Time: %d, Runstate: %d\n", p_stack[i].p_id, p_stack[i].p_et, p_stack[i].p_rs); // debug print
        // check if the process is live, if yes then execute it
        (p_stack[i].p_rs == 1) ? p_stack_exec(p_stack[i].p_id) : 0;
    }
}

// executes the code in the process
void p_stack_exec (int p_id) {
    // time for execution
    char* instructions = p_stack[p_id].p_inst; // get instructions
    int exec_time = p_stack[p_id].p_et; // get execution time
    // handle execution
    //printf("Executing Process ID: %d with Execution Time: %d\n", p_id, exec_time); // debug print
    //printf("Instructions: %s\n", instructions); // debug print
    if (instructions == NULL) {
        //printf("No instructions to execute for Process ID: %d\n", p_id); // debug print
    }
    else {
        compile(instructions);
    }
    
    // once executed, update the remaining p_et (IMPLEMENT)
    p_stack[p_id].p_et = 0;
}

// print the process stack
void stack_print () {
    puts(0, 0, BLACK, BRIGHT, "Process Stack:");
    for (int i = 0; i < sizeof(p_stack) / sizeof(p_stack[0]); ++i) {
        puts(0, i, BLACK, BRIGHT, toArray(p_stack[i].p_id)); // look into this, not really working
    }
}
#endif /* P_MAN_H */
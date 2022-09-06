#include "stdint.h"
#include "stdio.h"
#include "put.h"

/*
    Things To Note:
    - Once a process is created, it does not execute immediately. This must be called using: p_exec(p_id, p_et)
    - Process execution time is calculated based on the length of the input string, each character counting for 0.01 seconds.
*/


// structures
typedef struct process {
    int p_id; // process ID
    int p_et; // process execution time
    int p_rs; // process runstate, 0 = dead, 1 = live
    int p_t; // process ticket - when it will be executed, defined by the process stack
    char* p_inst; // process instructions
};

// constants
const float i = 0.0f;

// dynamic variables
int p_cnt = 0; // process count
struct process p_stack[1]; // process stack

// primary process handler (alternate method)
int p_id_handler (char action, int p_id) {
    (action == "d") ? p_destory(p_id) : (action == "f") ? p_freeze(p_id, 1, 0) : (action == "e") ? p_exec(p_id) : 0;
    return 0;
}

// create new process
void p_create (char* p_inst) {
    struct process p;
    p.p_id = p_cnt + 1;
    p.p_inst = p_inst;
    p.p_et = sizeof(p_inst) / 0.01f;
    p_stack[p.p_id] = p; // add process to process stack
}

// destory a process
void p_destory (int p_id) {
    p_stack[p_id].p_id = 000; // set the PID to 000, making it possible to re-use it in memory for creating new processes
    // the following is done to free memory
    p_stack[p_id].p_et = 0; // reset execution time to 0
    p_stack[p_id].p_rs = 0; // would normally make the process dead but not deleted but due to p_id = 000 this can be over-written
    p_stack[p_id].p_inst = ""; // remove all process instructions
    p_cnt = p_cnt - 1; // decrease process count
}

// freeze a process
void p_freeze (int p_id, float t, int dt) {
    (dt <= t * 1000) ? dt = dt + 1, p_freeze(p_id, t, dt) : 0;    
}

// repetative process execution (compilation)
void p_exec (int p_id) {   

    // first, add to the process stack so that it can be queued for execution by setting its state to live
    p_stack[p_id].p_rs = 1; // live

    char* instructions = p_stack[p_id].p_inst; // get instructions
    int exec_time = p_stack[p_id].p_et; // get execution time
    // handle execution
    (instructions[0] == "p" && instructions[0] == "u") ? puts(0, 0,  BLACK, BRIGHT, instructions) : 0; // puts() - improve this handler
    // once executed, update the remaining p_et (IMPLEMENT)
    p_stack[p_id].p_et = 0;
}

// update method for the process stack - redefine the execution schedule by updating processes ticket ID if p_rs is 1 (live)
void p_stack_update () {
}
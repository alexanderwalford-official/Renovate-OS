#ifndef P_MAN_H
#define P_MAN_H

#include "stdint.h"
#include "../drivers/video/VGA_linear.h"
#include "../lib/ren_comp.h"
#include "../lib/primary_definitions.h"

/*
    Things To Note:
    - Once a process is created, it does not execute immediately. This must be called using: p_exec(p_id, p_et).
    - Process execution time is calculated based on the length of the input string, each character counting for 0.01 seconds.
    - The process stack is automatically updated when the process is executed.
    - All processes will be executed after execution of an initial process is they are live.
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
int p_cnt = 0; // process count, automatic handler
struct process p_stack[256]; // process stack

// primary process handler (alternate method)
int p_id_handler (char* action, int p_id) {
    (action == "d") ? p_destory(p_id) : (action == "f") ? p_freeze(p_id, 1, 0) : (action == "e") ? p_exec(p_id) : (action == "p") ? p_print() : ret();
    return 0;
}

// create new process
void p_create (char* c_p_inst) {
    struct process p;
    p.p_id = p_cnt + 1;
    p.p_inst = c_p_inst;
    p.p_et = sizeof(c_p_inst) / 0.01f;
    p_stack[p.p_id] = p; // add process to process stack
}

// destory a process
void p_destory (int p_id) {
    p_stack[p_id].p_id = 000; // set the PID to 000, making it possible to re-use it in memory for creating new processes
    p_stack[p_id].p_et = 0; // reset execution time to 0
    p_stack[p_id].p_rs = 0; // would normally make the process dead but not deleted but due to p_id = 000 this can be over-written
    p_stack[p_id].p_inst = ""; // remove all process instructions
    p_cnt = p_cnt - 1; // decrease process count
}

// freeze a process (milliseconds)
void p_freeze (int p_id, float t, int dt) {

    /*
        Things to note:
        Currently, the objective is to make the operating system 'hang' when this is called.
        Once multi-threading has been implemented, this would have to be changed.
    */

    // loop until incremented to target
    (dt <= t * 1000) ? dt = dt + 1, p_freeze(p_id, t, dt) : ret();

    // use the system clock, H = hexidecimal, extended ASM: https://gcc.gnu.org/onlinedocs/gcc/Extended-Asm.html 
    // https://stackoverflow.com/questions/15201955/how-to-set-1-second-time-delay-at-assembly-language-8086 
    asm (
        "MOV %CX, $'FH' 0 \n"
        "MOV %DX, $'H' 4240 \n"
        "MOV %AH, $'H' " // or 86
        "INT $'H' " // or 15
        : "=r" (t)
    ); // currently not working as intended
}

// repetative process execution (compilation)
void p_exec (int p_id) {   
    // first, add to the process stack so that it can be queued for execution by setting its state to live
    p_stack[p_id].p_rs = 1; // live

    // update the process stack
    p_stack_update(0);

    // start the process stack executioner
    p_stack_runtime(0);

    // debugging message
    puts(0, 2, BRIGHT, BLACK, "[E:x0f1]");
}

// dynamic vars
int p_et_stack[];

// update method for the process stack
// redefine the execution schedule by updating processes ticket ID if p_rs is 1 (live)
// uses a linear sorting algorithm to order processes by execution time
void p_stack_update (int cn) {
    if (cn < sizeof(p_stack)) {
        if (p_stack[cn].p_rs == 1) {
            p_et_stack[cn] = p_stack[cn].p_et;
            if (cn != 0) {
                if (p_et_stack[cn] < p_et_stack[cn - 1]) {
                    // move the process closer to the execution point by swapping their struct values
                    p_stack[p_et_stack[cn] - 1] = p_stack[p_et_stack[cn]];
                    // move onto the next process
                    cn = cn + 1;
                    p_stack_update(cn);
                }
            }
            else {
                // move onto the next process as we don't have anything to compare it to
                cn = cn + 1;
                p_stack_update(cn);
            }
        }
        else {
            // process not live, move onto the next
            cn = cn + 1;
            p_stack_update(cn);
        }
    }
}

// actually calls each process in order of execution, requires implementation for execution that does not cause stack overflow
void p_stack_runtime (int c) {
    p_stack_exec(p_stack[c].p_id); // execute process
    // increment pointer if not reached max
    if (c != sizeof(p_stack)) {
        // increment
        c = c + 1;
        p_stack_runtime(c); // re-call method
    } 
}

// executes the code in the process
void p_stack_exec (int p_id) {
    // time for execution
    char* instructions = p_stack[p_id].p_inst; // get instructions
    int exec_time = p_stack[p_id].p_et; // get execution time
    // handle execution
    compile(instructions);
    // once executed, update the remaining p_et (IMPLEMENT)
    p_stack[p_id].p_et = 0;
}

// print the process stack
void p_print () {
    puts(0, 0, BLACK, BRIGHT, "Process Stack:");
    puts(0, 1, BLACK, BRIGHT, p_stack);
}
#endif /* P_MAN_H */
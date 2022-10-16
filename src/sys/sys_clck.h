/*
    SYS_CLCK_H
    This class file is responsible for handling all system sensetive operations including but not limited to:
    - Process timing.
    - Screen refresh rates.
    - Local system clock (24hr).

    Current design flaw considerations:
    - The operating system timing works based on CPU performance, faster = less proccess hanging
    - Only 1 thread at a time can be used.
    - Performance is less than to be desired - implement CPU clock cycle timing
    - Cannot use many branchless methods due to lvalues not being modifiable.
*/

#include <stdint.h>
#include "../lib/primary_definitions.h"

#ifndef SYS_CLOCK_H
#define SYS_CLOCK_H

bool p_clck_invokes[256]; // stores the states of up to 256 proccesses
bool sys_threads_status[4] = {true, true, true, true}; // eventual support for quad threads, only state 0 is used currently
int gt = 0; // global timer

// fires when a process is executed from the main class
void clck_invoke (int pid) {
    // if not invoked, invoke
    if (!p_clck_invokes[pid]) {
        p_clck_invokes[pid] = true;
    }
    clck_timing(0, pid); // update sys clock
}

// invoked every time a process is executed, increase value and increment global clock if needed
void clck_timing (int lt, int pid) {
    (lt == 60) ? gt = gt + 1 : 0;
    (p_clck_invokes[pid]) ? clck_timing(lt, pid) : 0;
    lt = lt + 1;
}

// used to time how long a thread should hang for
void clck_hang (float t, float lt) {
    sys_threads_status[0] = false;
    lt = lt + 0.1;

    if (lt < t) {
        clck_hang(t, lt);
    }
    else {
        sys_threads_status[0] = true; 
    }
}

#endif
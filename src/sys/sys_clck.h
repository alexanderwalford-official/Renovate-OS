/*
    SYS_CLCK_H
    This class file is responsible for handling all system sensetive operations.

    TO DO:
    Replace with time.h and clock_t method - this will enable realtime processes not per-processor dependant timings.
*/

#include <stdint.h>
#include <time.h>
#include "../lib/primary_definitions.h"
#include "../drivers/video/VGA_linear.h"

#ifndef SYS_CLOCK_H
#define SYS_CLOCK_H

bool sys_threads_status[4] = {true, false, false, false}; // eventual support for quad threads, only state 0 is used currently

// haults a thread for the desired time, we will later add a argument for thread ID and then will check if it is in use
void clck_hang (float t, float lt) {
    // start a system clock (IMPLEMENT)
    clock_t start_t, end_t;

    sys_threads_status[0] = false;
    lt = lt + 0.00013; 
    if (lt < t) { // there is a reason why this is not a branchless method! It's slower..
        clck_hang(t, lt);
    }
    else {
        sys_threads_status[0] = true; 
    }
}

#endif
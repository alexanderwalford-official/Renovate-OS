/*
    SYS_CLCK_H
    This class file is responsible for handling all system sensetive operations.
*/

#include <stdint.h>
#include "../lib/primary_definitions.h"
#include "../drivers/video/VGA_linear.h"

#ifndef SYS_CLOCK_H
#define SYS_CLOCK_H

bool sys_threads_status[4] = {true, false, false, false}; // eventual support for quad threads, only state 0 is used currently

// haults a thread for the desired time, we will later add a argument for thread ID and then will check if it is in use
void clck_hang (float t, float lt) {
    sys_threads_status[0] = false;
    lt = lt + 0.00013;
    if (lt < t) {
        clck_hang(t, lt);
    }
    else {
        sys_threads_status[0] = true; 
    }
}

#endif
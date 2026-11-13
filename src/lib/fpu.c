/*
    fpu.c - switching on the x87 FPU (see fpu.h)

    Control register 0 (CR0) has the switches, one bit each:

        bit 1  MP   "monitor co-processor": on, so WAIT instructions check the FPU
        bit 2  EM   "emulate": on means "there is no FPU, stop at every float instruction"
        bit 3  TS   "task switched": on stops at the first float instruction (for saving FPU state
                    when switching tasks, which AerOS doesn't do)
        bit 5  NE   "numeric error": on reports maths errors the modern way

    So EM and TS go off, MP and NE go on, and then "fninit" resets the FPU itself to its normal
    starting state (round to nearest, 64 bit precision, every error masked).
*/

#include "fpu.h"
#include <stdint.h>
#include <stdbool.h>

// switch the FPU on (see the top of this file), so float and double maths can be used. Only the
// first call does anything
void fpu_enable (void) {
    static bool enabled = false;
    if (enabled) {
        return;
    }
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1u << 2) | (1u << 3));   // EM and TS off
    cr0 |= (1u << 1) | (1u << 5);      // MP and NE on
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile ("fninit");
    enabled = true;
}

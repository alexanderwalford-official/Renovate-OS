// this file's entire purpose is to allow the system to output specific frequency sounds for debugging
// https://wiki.osdev.org/PC_Speaker

#ifndef TONE_H
#define TONE_H

#include "stdint.h"

void beep(int arg) {
    // 0x61 = 1
    uint8_t *const In = (uint8_t*) 0x61;

    // find a way of setting memory address values
}

#endif /* TONE_H */
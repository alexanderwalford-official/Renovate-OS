/*
    PS2.h
    Alexander Walford 2022

    This class file was developed from the brilliant information over at:
    https://wiki.osdev.org/PS/2_Keyboard
*/

#ifndef PS2_H
#define PS2_H

#include "stdint.h"
#include "../../lib/sys_primary_definitions.h"

// use uint16_t (16 bit unsigned integer) for keyboard signals

// get keyboard input
int init_ps2 () {
    uint16_t *const EnableScan = (uint16_t*) 0xF4; // enable key scanning
    return 0;
}

int dinit_ps2 () {
    uint16_t *const DisableScan = (uint16_t*) 0xF5; // disable key scanning
    return 0;
}

char ps2_get_char () {
    // retrieve each keypress signal from the PS2 port
    char* c = ps2_priv_char();
    (c == "") ? ps2_get_char() : 0; // loop if null
    return c;
}

char ps2_get_str (char* str, int cn) {
    // retrieve each keypress signal from the PS2 port and compile it to a string
    char* c = ps2_priv_char();
    if (c != "\n") {
        // continue
        if (c != "") {
            // add if not nul;
            str[cn] = c;
            cn = cn + 1;
        }
        // loop
        ps2_get_str(str, cn);
    }
    else {
        // end as enter has been pressed
        return str;
    }
}

char ps2_priv_char () {
    char* c;
    // read memory address and see if true
    uint16_t *const SP = (uint16_t*) 0x39; // SPACE
    uint16_t *const ENT = (uint16_t*) 0x1C; // ENTER
    uint16_t *const F2 = (uint16_t*) 0x3C; // F2
    uint16_t *const Y = (uint16_t*) 0x15; // Y
    uint16_t *const N = (uint16_t*) 0x32; // N

    (SP) ? c = ' ' : (Y) ? c = 'Y' : (N) ? c = 'N' : (ENT) ? c = "\n" : (F2) ? c = "F2" : c == "";
    return c;
}

#endif /* PS2_H */
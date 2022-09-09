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

// constant vars
const char* codes_char[5] = {" ", "\n", "F2", "Y", "N"};
const uint16_t codes[5] = {0x39, 0x1c, 0x3C, 0x15, 0x32};


// get keyboard input
int init_ps2 () {
    uint16_t *const EnableScan = (uint16_t*) 0xF4; // enable key scanning
    return 0;
}

int dinit_ps2 () {
    uint16_t *const DisableScan = (uint16_t*) 0xF5; // disable key scanning
    return 0;
}

char* ps2_priv_char (int cn) {
    char* c = "";
    if (cn < sizeof(codes)) {
        // read memory address and see if true
        uint16_t *const ps2_addr = (uint16_t*) codes[cn];
        if (ps2_addr) {
            // the code is true, translate it into a char and return it
            c = codes_char[cn];
            return c;
        }
        else {
            // keep going until you find an active code
            cn = cn + 1;
            ps2_priv_char(cn);
        }
    }
    else {
        // nothing pressed
        return c;
    }
}

char* ps2_get_char () {
    // retrieve each keypress signal from the PS2 port
    char* c = ps2_priv_char(0);
    (c == "") ? ps2_get_char() : 0; // loop if null
    return c;
}

char* ps2_get_str (char* str, int cn) {
    // retrieve each keypress signal from the PS2 port and compile it to a string
    char* c = ps2_priv_char(0);
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

#endif /* PS2_H */
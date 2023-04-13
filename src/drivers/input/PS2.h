/*
    PS2.h
    Alexander Walford 2023

    This class file was developed from information over at:
    https://wiki.osdev.org/PS/2_Keyboard
*/

#ifndef PS2_H
#define PS2_H

#include "stdint.h"
#include "GIO.h"
#include "../../lib/primary_definitions.h"

// use uint16_t (16 bit unsigned integer) for 16bit keyboard signals

// possible better option using NASM Assembly? https://stackoverflow.com/questions/523185/how-do-i-get-user-input-with-nasm 


// constant vars
const char* codes_char[6] = {" ", "\n", "F2", "y", "n", "F12"};
const uint16_t codes[6] = {0x39, 0x1c, 0x3C, 0x15, 0x32, 0x58};

// get keyboard input sect


int init_ps2 () {
    // reset keyboard controller
    /*
    commented due to too many references for mov error
    __asm(
    "mov rcx, 0xFE"
    "out 0x64, rcx"
    );
    */
    set_byte(0xF4); // enable key scanning
    return 0;
}

// disable keyboard scanning
int dinit_ps2 () {
    set_byte(0xF5); // disable key scanning
    return 0;
}

// private method for getting each key press
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

// get pressed char and return it
char* ps2_get_char () {
    char* c = ps2_priv_char(0);
    (c == "") ? ps2_get_char() : 0; // loop if null
    return c;
}

// get keyboard input as a string until ENTER is pressed
char* ps2_get_str (char* str, int cn) {
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
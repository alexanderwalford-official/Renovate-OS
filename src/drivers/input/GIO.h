/*
    Alexander Walford 2023
    General purpose I/O driver for interacting with devices. 
*/
#include "stdint.h"

#ifndef GIO_H
#define GIO_H

uint16_t *const get_byte(uint16_t* addr) {
    uint16_t *const value = (uint16_t*) addr;
    return value;
}

int set_byte(uint16_t* addr) {
    uint16_t *const var = (uint16_t*) addr;
    return 0;
}


#endif
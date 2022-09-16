// For SATA interfaces only
// https://wiki.osdev.org/SATA

#include "stdint.h"

int drive_init (char arg) {
    (arg == 'E') ? drive_check_err() : 0;
    return 0;
}

char* drive_check_err () {

    char* response = "";

    // IDENTIFY COMMAND, CREATE MEMORY ENTRY FOR UNSIGNED 16 BIT INTEGER
    uint16_t *const D_ID = (uint16_t*) 0xEC;

    // error should be present in the ERR bit of the status register and a pair of signature bytes
    // umm.. hm.. how do I do that.. ?

    // primary ATA bus
    // IO ports
    uint16_t *const D_IO_1 = (uint16_t*) 0x1f4;
    uint16_t *const D_IO_1 = (uint16_t*) 0x1F5;

    // expect val 0x3C
    // err.. ? but how do we read it??


    return response;
}
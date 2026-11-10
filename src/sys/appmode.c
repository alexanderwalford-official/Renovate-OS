/*
    appmode.c - app mode (see appmode.h)
*/

#include "appmode.h"
#include "net.h"
#include "../lib/aer_stdlib.h"
#include "../drivers/serial/serial.h"
#include "../lib/aer_stdio.h"
#include "../lib/aer_string.h"

static appmode_t now;

const char* appmode_part_name (uint32_t part) {
    switch (part) {
        case APP_GRAPHICS: return "graphics";
        case APP_SOUND:    return "sound";
        case APP_VIDEO:    return "video";
        case APP_FILES:    return "files";
        case APP_NETWORK:  return "network";
        default:           return "?";
    }
}

void appmode_describe (uint32_t uses, char* out, size_t size) {
    out[0] = '\0';
    for (int bit = 0; bit < APP_PARTS; bit++) {
        uint32_t part = 1u << bit;
        if (uses & part) {
            if (out[0] != '\0') {
                scat(out, ", ", size);
            }
            scat(out, appmode_part_name(part), size);
        }
    }
    if (out[0] == '\0') {
        scpy(out, "only the screen and keyboard", size);
    }
}

// put away the parts `uses` leaves out, and return how many bytes that gave back
static size_t put_away (uint32_t uses) {
    size_t before = heap_used();

    // the parts that keep memory while they aren't used (see the table in appmode.h)
    if (!(uses & APP_NETWORK)) {
        net_rest();
    }

    size_t after = heap_used();
    return before > after ? before - after : 0;
}

void appmode_again (void) {
    now.freed += put_away(now.uses);
    now.free = heap_total() - heap_used();
}

void appmode_begin (uint32_t uses) {
    now.ran = true;
    now.uses = uses;
    now.freed = put_away(uses);
    now.free = heap_total() - heap_used();

    // a line on the serial port, for anyone watching (QEMU's -serial stdio)
    char line[96];
    sfmt(line, sizeof(line), "app mode: profile %02x, %u KB given back, %u KB free\r\n",
         (unsigned int)uses, (unsigned int)(now.freed / 1024), (unsigned int)(now.free / 1024));
    serial_write(line);
}

const appmode_t* appmode_now (void) {
    return &now;
}

/*
    appmode.c - app mode (see appmode.h)
*/

#include "appmode.h"
#include "net.h"
#include "../lib/aer_stdlib.h"
#include "../drivers/serial/serial.h"
#include "../lib/aer_stdio.h"
#include "../lib/aer_string.h"
#include "trace.h"

// what the last app mode program asked for, and what that gave back (read with appmode_now)
static appmode_t now;

// the name of one APP_ part (a single bit), for messages
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

// the parts in a `uses` profile as words, e.g. "graphics, sound"
void appmode_describe (uint32_t uses, char* out, size_t size) {
    out[0] = '\0';
    // each part is one bit of `uses`, so test them one at a time
    for (int bit = 0; bit < APP_PARTS; bit++) {
        uint32_t part = 1u << bit;
        if (uses & part) {
            // a comma between names, but not before the first
            if (out[0] != '\0') {
                scat(out, ", ", size);
            }
            scat(out, appmode_part_name(part), size);
        }
    }
    // no parts at all: every program still has the screen and keyboard
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

    // if the heap somehow grew instead, before - after would wrap round to a huge size_t, so
    // that counts as nothing given back
    size_t after = heap_used();
    return before > after ? before - after : 0;
}

// put away again whatever the program doesn't use but has started since, adding to the total
void appmode_again (void) {
    now.freed += put_away(now.uses);
    now.free = heap_total() - heap_used();
}

// a program is starting in app mode with the parts in `uses`: put the rest away, see appmode.h
void appmode_begin (uint32_t uses) {
    // remember what was asked for and how much memory it gave back, for appmode_now
    now.ran = true;
    now.uses = uses;
    now.freed = put_away(uses);
    now.free = heap_total() - heap_used();

    // a line on the serial port, for anyone watching (QEMU's -serial stdio)
    char line[96];
    sfmt(line, sizeof(line), "app mode: profile %02x, %u KB given back, %u KB free\r\n",
         (unsigned int)uses, (unsigned int)(now.freed / 1024), (unsigned int)(now.free / 1024));
    serial_write(line);

    // and for SHOW: what the program was found to use, and what that left room for
    char parts[96];
    appmode_describe(uses, parts, sizeof(parts));
    TRACE("read the whole program first: it uses %s, so the rest was put away (%u KB back, %u KB free)",
          parts, (unsigned int)(now.freed / 1024), (unsigned int)(now.free / 1024));
}

// what app mode last did (now.ran is false if no program has used it yet)
const appmode_t* appmode_now (void) {
    return &now;
}

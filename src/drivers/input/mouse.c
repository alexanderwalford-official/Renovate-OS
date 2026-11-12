/*
    mouse.c - the PS/2 mouse driver (see mouse.h for how the mouse talks)

    Switching on (mouse_start):
      1. Enable the controller's second port (command 0xA8).
      2. In the controller's configuration byte, turn on IRQ12 (bit 1) and the second port's
         clock (bit 5 set means it is stopped).
      3. Send the mouse two commands, each through controller command 0xD4 ("the next data byte is
         for the second port"): 0xF6 sets the default settings, 0xF4 starts it sending packets.
         The mouse answers each with 0xFA ("acknowledged").
      4. Point IRQ12 and the mouse sink in IO.c at this driver, and unmask IRQ12.
    All of it runs with interrupts off, so the answers are read here and not by an IRQ handler.

    Switching off (mouse_stop) is the same backwards, finishing with any mouse bytes still in the
    controller thrown away: one left there would stop key presses getting through.

    The IRQ handler only adds the movement up. Programs read it with interrupts off for a moment,
    so they never see a half updated pair of numbers.
*/

#include <stddef.h>
#include "mouse.h"
#include "IO.h"
#include "GIO.h"
#include "../../sys/interrupts.h"

// controller commands (written to KBC_COMMAND)
#define KBC_READ_CONFIG   0x20
#define KBC_WRITE_CONFIG  0x60
#define KBC_DISABLE_MOUSE 0xA7
#define KBC_ENABLE_MOUSE  0xA8
#define KBC_TO_MOUSE      0xD4 // the next byte written to KBC_DATA goes to the mouse

// configuration byte bits for the second port
#define CONFIG_MOUSE_IRQ       0x02 // raise IRQ12 when a mouse byte arrives
#define CONFIG_MOUSE_CLOCK_OFF 0x20 // the second port's clock is stopped

// mouse commands, and its answer to each
#define MOUSE_DEFAULTS       0xF6
#define MOUSE_REPORTING_ON   0xF4
#define MOUSE_REPORTING_OFF  0xF5
#define MOUSE_ACK            0xFA

#define MOUSE_IRQ 12

static bool running = false;
static bool missing = false; // looked for once and not found

// the packet being put together, and how many of its 3 bytes have arrived
static uint8_t packet[3];
static int have = 0;

// written by the IRQ handler, read by programs (hence volatile: always read from memory)
static volatile int pointer_x = 0, pointer_y = 0;
static volatile int area_width = 640, area_height = 480;
static volatile int held = 0;
static volatile int moved_x = 0, moved_y = 0;

// is the mouse on right now?
bool mouse_ready(void) {
    return running;
}

// keep a value between low and high
static int clamp(int value, int low, int high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

// one byte from the mouse (the sink ps2_service calls, inside the IRQ). Three make a packet
static void mouse_byte(uint8_t byte) {
    // The first byte of a packet always has bit 3 set. A byte without it can't start one, so a
    // byte must have been lost: drop this one and wait for a byte that can
    if (have == 0 && !(byte & 0x08)) {
        return;
    }
    packet[have++] = byte;
    if (have < 3) {
        return;
    }
    have = 0;

    uint8_t flags = packet[0];
    held = flags & 0x07;
    // an overflow bit means it moved further than a packet can say: the numbers are wrong, so
    // the movement is left out (the buttons above are still right)
    if (flags & 0xC0) {
        return;
    }
    // Each movement is 9 bits: the byte, with its sign as bit 4 (x) or 5 (y) of the flags.
    // Shifting the sign up to bit 8 and taking it away turns 0x1FF into -1, and so on
    int dx = (int)packet[1] - ((flags << 4) & 0x100);
    int dy = (int)packet[2] - ((flags << 3) & 0x100);
    // the mouse counts up as positive, the screen counts down
    dy = -dy;

    moved_x += dx;
    moved_y += dy;
    pointer_x = clamp(pointer_x + dx, 0, area_width - 1);
    pointer_y = clamp(pointer_y + dy, 0, area_height - 1);
}

// IRQ12: a byte from the mouse is waiting. ps2_service reads it and hands it to mouse_byte
static void mouse_irq(struct regs* r) {
    (void)r;
    ps2_service();
}

// send the mouse a command and wait for its "acknowledged". Interrupts are off, so this reads
// the answer itself. A key pressed meanwhile is passed on to the keyboard rather than lost, and
// a byte left over from a packet is skipped
static bool mouse_send(uint8_t command) {
    if (!kbc_write(KBC_COMMAND, KBC_TO_MOUSE) || !kbc_write(KBC_DATA, command)) {
        return false;
    }
    // a few bytes at most can come before the answer
    for (int tries = 0; tries < 8; tries++) {
        if (!kbc_wait_read()) {
            return false;
        }
        if (!(inb(KBC_STATUS) & KBC_FROM_MOUSE)) {
            ps2_service(); // a key press: let the keyboard have it
            continue;
        }
        if (inb(KBC_DATA) == MOUSE_ACK) {
            return true;
        }
    }
    return false;
}

// turn the second port's bits in the controller's configuration byte on or off
static bool set_mouse_config(bool on) {
    if (!kbc_write(KBC_COMMAND, KBC_READ_CONFIG) || !kbc_wait_read()) {
        return false;
    }
    uint8_t config = inb(KBC_DATA);
    if (on) {
        config |= CONFIG_MOUSE_IRQ;
        config &= (uint8_t)~CONFIG_MOUSE_CLOCK_OFF;
    } else {
        config &= (uint8_t)~CONFIG_MOUSE_IRQ;
        config |= CONFIG_MOUSE_CLOCK_OFF;
    }
    return kbc_write(KBC_COMMAND, KBC_WRITE_CONFIG) && kbc_write(KBC_DATA, config);
}

// throw away any mouse bytes still waiting in the controller (see the top of this file)
static void drop_mouse_bytes(void) {
    for (int i = 0; i < 16 && (inb(KBC_STATUS) & (KBC_OUTPUT_FULL | KBC_FROM_MOUSE)) ==
                                   (KBC_OUTPUT_FULL | KBC_FROM_MOUSE); i++) {
        inb(KBC_DATA);
    }
}

// switch the mouse on, see mouse.h and the steps at the top of this file
bool mouse_start(void) {
    if (running) {
        return true;
    }
    if (missing) {
        return false;
    }
    bool were_on = interrupts_enabled();
    interrupts_disable();

    // steps 1 to 3: the port, the configuration byte, then the mouse itself
    bool ok = kbc_write(KBC_COMMAND, KBC_ENABLE_MOUSE) && set_mouse_config(true) &&
              mouse_send(MOUSE_DEFAULTS) && mouse_send(MOUSE_REPORTING_ON);

    if (ok) {
        // step 4: start with no movement and no buttons, then let the bytes in
        have = 0;
        held = 0;
        moved_x = 0;
        moved_y = 0;
        kb_set_mouse_sink(mouse_byte);
        irq_install_handler(MOUSE_IRQ, mouse_irq);
        pic_unmask(MOUSE_IRQ);
        running = true;
    } else {
        // no mouse answered: put the controller back as it was and don't look again
        set_mouse_config(false);
        kbc_write(KBC_COMMAND, KBC_DISABLE_MOUSE);
        drop_mouse_bytes();
        missing = true;
    }

    if (were_on) {
        interrupts_enable();
    }
    return ok;
}

// switch the mouse off again (see the top of this file)
void mouse_stop(void) {
    if (!running) {
        return;
    }
    bool were_on = interrupts_enabled();
    interrupts_disable();

    // stop the mouse sending first, then close the port and its IRQ behind it
    mouse_send(MOUSE_REPORTING_OFF);
    set_mouse_config(false);
    kbc_write(KBC_COMMAND, KBC_DISABLE_MOUSE);
    pic_mask(MOUSE_IRQ);
    irq_uninstall_handler(MOUSE_IRQ);
    kb_set_mouse_sink(NULL);
    drop_mouse_bytes();
    running = false;

    if (were_on) {
        interrupts_enable();
    }
}

// the area the pointer stays in, with the pointer put in its middle
void mouse_set_area(int width, int height) {
    bool were_on = interrupts_enabled();
    interrupts_disable();
    area_width = width > 0 ? width : 1;
    area_height = height > 0 ? height : 1;
    pointer_x = area_width / 2;
    pointer_y = area_height / 2;
    if (were_on) {
        interrupts_enable();
    }
}

// where the pointer is and which buttons are held (read together, with interrupts off)
void mouse_position(int* x, int* y, int* buttons) {
    bool were_on = interrupts_enabled();
    interrupts_disable();
    *x = pointer_x;
    *y = pointer_y;
    *buttons = held;
    if (were_on) {
        interrupts_enable();
    }
}

// the movement since the last call, then back to 0 (read and cleared with interrupts off, so a
// packet arriving in between isn't lost)
void mouse_motion(int* dx, int* dy) {
    bool were_on = interrupts_enabled();
    interrupts_disable();
    *dx = moved_x;
    *dy = moved_y;
    moved_x = 0;
    moved_y = 0;
    if (were_on) {
        interrupts_enable();
    }
}

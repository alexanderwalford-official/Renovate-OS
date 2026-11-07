// this file's entire purpose is to allow the system to output specific frequency sounds for debugging
// https://wiki.osdev.org/PC_Speaker
//
// The PC speaker is driven by PIT channel 2. 0x61 is an I/O PORT (not a memory address), so it is
// read and written with inb / outb: bits 0 and 1 connect the PIT to the speaker.
//
// QEMU only plays the sound when started with an audio backend, e.g.
//     qemu-system-i386 -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0 ...

#ifndef TONE_H
#define TONE_H

#include <stdint.h>
#include "../input/GIO.h"
#include "../../sys/sys_clck.h"

#define SPEAKER_PORT     0x61
#define PIT_CHANNEL2     0x42
#define BEEP_DEFAULT_HZ  1000
#define BEEP_DEFAULT_MS  100

// start playing a tone of the given frequency (Hz)
void tone_play(uint32_t frequency) {
    if (frequency == 0) {
        return;
    }
    uint32_t divisor = PIT_FREQUENCY / frequency;
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    outb(PIT_COMMAND, 0xB6); // channel 2, lobyte/hibyte, mode 3 (square wave)
    outb(PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)((divisor >> 8) & 0xFF));

    uint8_t state = inb(SPEAKER_PORT);
    if ((state & 3) != 3) {
        outb(SPEAKER_PORT, state | 3); // connect the speaker
    }
}

// stop the speaker
void tone_stop(void) {
    outb(SPEAKER_PORT, inb(SPEAKER_PORT) & 0xFC);
}

// play a tone for a number of milliseconds
void tone(uint32_t frequency, uint32_t ms) {
    tone_play(frequency);
    clck_hang((long)ms);
    tone_stop();
}

// beep once: arg 0 = the default beep, otherwise arg is the frequency in Hz
void beep(int arg) {
    tone(arg > 0 ? (uint32_t)arg : BEEP_DEFAULT_HZ, BEEP_DEFAULT_MS);
}

#endif /* TONE_H */

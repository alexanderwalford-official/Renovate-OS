// this file's entire purpose is to allow the system to output specific frequency sounds for debugging
// https://wiki.osdev.org/PC_Speaker
//
// QEMU only plays the sound when started with an audio backend, e.g.
//     qemu-system-i386 -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0 ...
//
// The PC speaker is the little beeper that has been in PCs since 1981. It cannot play samples, it
// can only be pushed in and out, so the trick is to let the timer chip (the PIT) do the pushing:
// PIT channel 2 generates a square wave at whatever frequency we ask for, and a gate in the
// system control port (0x61) connects that wave to the speaker. tone.c has the details.
// Used by the "beep" terminal command and as an audible signal from the rest of the OS.

#ifndef TONE_H
#define TONE_H

#include <stdint.h>

// start playing a tone of the given frequency (Hz), it keeps playing until tone_stop()
// returns straight away, the hardware keeps the sound going on its own. 0 Hz does nothing.
void tone_play(uint32_t frequency);

// silence the speaker (disconnects it from the timer, the timer itself keeps running)
void tone_stop(void);

// play a tone for a number of milliseconds
// blocks: the caller sleeps for the whole duration, then the speaker is switched off
void tone(uint32_t frequency, uint32_t ms);

// beep once: arg 0 = the default beep, otherwise arg is the frequency in Hz
// (negative values also give the default beep, every beep lasts 100ms)
void beep(int arg);

#endif /* TONE_H */

// this file's entire purpose is to allow the system to output specific frequency sounds for debugging
// https://wiki.osdev.org/PC_Speaker
//
// QEMU only plays the sound when started with an audio backend, e.g.
//     qemu-system-i386 -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0 ...

#ifndef TONE_H
#define TONE_H

#include <stdint.h>

// start playing a tone of the given frequency (Hz), it keeps playing until tone_stop()
void tone_play(uint32_t frequency);
void tone_stop(void);

// play a tone for a number of milliseconds
void tone(uint32_t frequency, uint32_t ms);

// beep once: arg 0 = the default beep, otherwise arg is the frequency in Hz
void beep(int arg);

#endif /* TONE_H */

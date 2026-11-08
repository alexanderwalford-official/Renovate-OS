// PC speaker tones
// https://wiki.osdev.org/PC_Speaker
//
// The PC speaker is driven by PIT channel 2. 0x61 is an I/O PORT (not a memory address), so it is
// read and written with inb / outb: bits 0 and 1 connect the PIT to the speaker.
//
// How it works:
// - The PIT (Programmable Interval Timer, an Intel 8253 / 8254) counts down from a "divisor" at a
//   fixed input clock of 1193182 Hz (PIT_FREQUENCY). It has three channels: channel 0 is the
//   system timer behind IRQ0 (see sys/sys_clck.c), channel 1 used to refresh old DRAM, and
//   channel 2's output is wired to the speaker.
// - In mode 3 a channel outputs a square wave of PIT_FREQUENCY / divisor Hz, so to play a note we
//   work out divisor = PIT_FREQUENCY / frequency and load it into channel 2.
// - Port 0x61 ("system control port B") holds the two switches between the PIT and the speaker:
//     bit 0 = the gate of PIT channel 2 (1 = let channel 2 count)
//     bit 1 = speaker data enable (1 = connect channel 2's output to the speaker)
//   The other bits of 0x61 belong to unrelated hardware (NMI / parity checks, status flags), so the
//   port is always read, modified and written back instead of overwritten.
// https://wiki.osdev.org/Programmable_Interval_Timer

#include "tone.h"

#include <stdint.h>
#include "../input/GIO.h"
#include "../../sys/sys_clck.h"

#define SPEAKER_PORT     0x61   // system control port B (speaker gate bits 0 and 1)
#define PIT_CHANNEL2     0x42   // data port of PIT channel 2 (PIT_COMMAND 0x43 comes from sys_clck.h)
#define BEEP_DEFAULT_HZ  1000   // pitch of beep(0)
#define BEEP_DEFAULT_MS  100    // length of every beep

// program PIT channel 2 for a square wave at frequency Hz and connect it to the speaker
// frequency: pitch in Hz, 0 is ignored (it would divide by zero below)
// the sound keeps going until tone_stop() because the hardware generates it, not this code
void tone_play(uint32_t frequency) {
    if (frequency == 0) {
        return;
    }
    uint32_t divisor = PIT_FREQUENCY / frequency;
    // the divisor register is only 16 bits, so frequencies under 19 Hz are clamped to the lowest
    // pitch the PIT can make (1193182 / 65535 = about 18.2 Hz)
    // gotcha: frequencies above PIT_FREQUENCY give divisor 0, which the PIT treats as 65536 (also ~18 Hz)
    if (divisor > 0xFFFF) {
        divisor = 0xFFFF;
    }

    // 0xB6 = 10 11 011 0: bits 7-6 = channel 2, bits 5-4 = send the divisor low byte then high byte,
    // bits 3-1 = mode 3 (square wave generator), bit 0 = plain binary counting (not BCD)
    outb(PIT_COMMAND, 0xB6); // channel 2, lobyte/hibyte, mode 3 (square wave)
    // the divisor goes to the channel's data port one byte at a time, low byte first
    outb(PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)(divisor >> 8));

    // set bits 0 and 1 of port 0x61 (gate + speaker enable), keeping every other bit as it was,
    // and skip the write if both are already on (e.g. when changing note without a tone_stop)
    uint8_t state = inb(SPEAKER_PORT);
    if ((state & 3) != 3) {
        outb(SPEAKER_PORT, state | 3); // connect the speaker
    }
}

// switch the speaker off by clearing bits 0 and 1 of port 0x61 (0xFC = every bit except those two)
void tone_stop(void) {
    outb(SPEAKER_PORT, inb(SPEAKER_PORT) & 0xFC);
}

// play frequency Hz for ms milliseconds, then go quiet again
// clck_hang sleeps on the 1000 Hz system timer (or, before the timer is running, a rough busy wait),
// so the caller is stuck here for the whole tone
void tone(uint32_t frequency, uint32_t ms) {
    tone_play(frequency);
    clck_hang((long)ms);
    tone_stop();
}

// a short beep: arg is the pitch in Hz, anything 0 or below means the default 1000 Hz
// (the terminal's "beep" command passes atoi() of its argument straight in, so no argument = 0)
void beep(int arg) {
    tone(arg > 0 ? (uint32_t)arg : BEEP_DEFAULT_HZ, BEEP_DEFAULT_MS);
}

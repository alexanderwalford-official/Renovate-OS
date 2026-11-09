/*
    ac97.h
    Intel AC'97 sound card driver, plays 16 bit stereo samples.

    AC'97 is what QEMU (-device AC97), VirtualBox and most PCs from around 1997 to 2006 have.
    Newer PCs use HD Audio instead, which this driver does not support.

    The card has two sets of I/O ports:
      - the mixer (NAM, BAR 0): volumes and the sample rate
      - the bus master (NABM, BAR 1): plays sound straight out of memory by itself (DMA)

    Playing works through a list of 32 buffer descriptors, each pointing at a piece of the sound
    in memory. The card works through the list in a circle, and as it finishes each piece
    ac97_play points that descriptor at the next piece of sound, until it has all been played.

    https://wiki.osdev.org/AC97
*/
#ifndef AC97_H
#define AC97_H

#include <stdint.h>
#include <stdbool.h>

// true once ac97_init() found and set up a card
extern bool ac97_ready;

// find the card and get it ready, false if there isn't one
bool ac97_init (void);

// a description of the card for the boot log, e.g. "AC'97 (PCI 00:04.0)"
const char* ac97_name (void);

// ask for a sample rate, returns the rate the card will really use
// (cards without variable rate support always play at 48000 Hz)
uint32_t ac97_set_rate (uint32_t hz);

// the most ac97_start can queue: 32 descriptors of 32767 stereo frames, about 21 seconds at 48000 Hz
#define AC97_START_MAX (32u * 32767u)

// start playing count stereo frames in the background and return straight away
// the frames must stay in memory until the sound ends or ac97_stop() is called, anything past
// AC97_START_MAX is left out. Returns how many frames were queued
uint32_t ac97_start (const int16_t* frames, uint32_t count);

// stop whatever is playing
void ac97_stop (void);

// play count stereo frames (left, right, left, right... 16 bit signed) and wait until they finish
// should_stop is checked while playing, returning true stops the sound early (it may be NULL)
// returns false if the card stopped responding
bool ac97_play (const int16_t* frames, uint32_t count, bool (*should_stop)(void));

// STREAMING: for sound that is made as it plays (a mixer, a video's soundtrack). The driver
// keeps a few small blocks queued on the card, and asks the fill function for each new block:
// fill(frames, count) must write count stereo frames (2 * count samples) at AC97_STREAM_RATE.
// ac97_stream_service() does the asking. It is meant to be called from the timer interrupt every
// couple of milliseconds (sys/mixer.c does that), so the fill function runs inside an interrupt:
// it must be quick and must not wait for anything.
// ac97_play, ac97_start and ac97_stop all end streaming, as the card plays one thing at a time
#define AC97_STREAM_RATE 48000 // every card can play this rate
typedef void (*ac97_fill_fn)(int16_t* frames, uint32_t count);
bool ac97_stream_start (ac97_fill_fn fill); // false if there is no card
void ac97_stream_service (void);
void ac97_stream_stop (void);
bool ac97_streaming (void);

#endif /* AC97_H */

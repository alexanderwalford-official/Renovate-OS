/*
    mixer.h - several sounds at once (for games, and the soundtrack of a video)

    PLY and play() play one sound from start to finish and wait for it. A game needs more: music
    looping in the background while sound effects start and stop on top of it, and the program
    carrying on (drawing, reading keys) while they play.

    The mixer has MIXER_VOICES "voices". Each voice plays one sound, from memory, at its own
    volume, once or round and round. Every 10ms or so the sound card needs its next little block
    of sound; the mixer makes it by adding up what every voice has at that moment:

        voice 0  music.wav   ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ (looping)
        voice 1  jump.wav         ~~~~~~
        voice 2  coin.wav                  ~~~
                                  |
                                  v   added up, sample by sample, then clipped to 16 bits
                             to the sound card

    It all happens in the background, from the timer interrupt (see drivers/audio/ac97.h,
    "streaming"), so the program doesn't have to do anything to keep the sound going.

    Sounds must be 16 bit stereo at MIXER_RATE (sound_load() in sys/sound.h gives exactly that)
    and must stay in memory while a voice is playing them.
*/
#ifndef MIXER_H
#define MIXER_H

#include <stdint.h>
#include <stdbool.h>

#define MIXER_VOICES 8        // how many sounds can play at once
#define MIXER_RATE   48000    // the rate the mixer plays at (AC97_STREAM_RATE)
#define MIXER_FULL_VOLUME 256 // volume runs from 0 (silent) to this (as loud as recorded)

// start playing `count` stereo frames, once or (loop) round and round, at a volume from 0 to
// MIXER_FULL_VOLUME. Returns a voice number for mixer_stop / mixer_playing, or -1 if there is
// no sound card or all the voices are busy
int mixer_play (const int16_t* frames, uint32_t count, bool loop, int volume);

// stop one voice (a voice that has already finished, or a wrong number, is fine)
void mixer_stop (int voice);

// is this voice still playing? (false once a sound played once has reached its end)
bool mixer_playing (int voice);

// stop every voice and give the sound card back (programs do this when they end)
void mixer_stop_all (void);

// pause a voice (it keeps its place and plays nothing) or carry it on again
void mixer_pause (int voice, bool paused);

// how many frames this voice has played so far (for a stream: since it was opened or last
// cleared). The sound card is a little behind this: see mixer_delay()
uint32_t mixer_played (int voice);

// roughly how many frames the sound card has been given but not played yet. A frame counted
// by mixer_played() is heard about this many frames later (about 85ms)
uint32_t mixer_delay (void);

// STREAMS: sound that is made while it plays, like a video's sound track. The voice has a ring
// buffer of (at least) buffer_frames frames; the program keeps it topped up with
// mixer_stream_write, and the voice plays whatever is waiting there. If the program falls
// behind, the voice is silent until more arrives (it doesn't stop). Close it with mixer_stop.
// Returns a voice number, or -1 (no sound card, no free voice, or not enough memory)
int mixer_stream_open (uint32_t buffer_frames, int volume);

// how many frames would fit in the stream's buffer right now
uint32_t mixer_stream_room (int voice);

// how many frames are waiting in the buffer, not played yet
uint32_t mixer_stream_queued (int voice);

// add `count` stereo frames (MIXER_RATE, 16 bit) to the end of the stream. Returns how many
// fitted (fewer than count if the buffer is nearly full)
uint32_t mixer_stream_write (int voice, const int16_t* frames, uint32_t count);

// throw away everything waiting and start the played count again from 0 (after a seek)
void mixer_stream_clear (int voice);

#endif /* MIXER_H */

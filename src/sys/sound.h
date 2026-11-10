/*
    sound.h
    Playing WAV and MP3 files through the sound card.

    A file is decoded into 16 bit stereo samples in memory, converted to a sample rate the card
    can play if needed, then handed to the AC'97 driver (drivers/audio/ac97.h).

    WAV: plain PCM, 8, 16, 24 or 32 bits, mono or stereo, any sample rate.
    MP3: anything minimp3 understands (MPEG 1, 2 and 2.5 layer 3, and layers 1 and 2).
*/
#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>
#include <stdint.h>

#define SOUND_OK             0
#define SOUND_ERR_NO_CARD   -1
#define SOUND_ERR_NOT_FOUND -2
#define SOUND_ERR_FORMAT    -3 // not a WAV or MP3 we can play
#define SOUND_ERR_NO_MEMORY -4
#define SOUND_ERR_CARD      -5 // the card stopped responding

// play a file from start to finish (or until should_stop, which may be NULL, returns true)
int sound_play_file (const char* path, bool (*should_stop)(void));

// start the startup sound and return straight away (it plays on while the OS carries on booting)
// it is /sys/startup.wav, which can be replaced with any WAV or MP3 (or an empty file for silence).
// The built-in copy plays when there is no disk, and is put back if the file is deleted
void sound_startup (void);

// decode a WAV or MP3 file into 16 bit stereo at the mixer's rate (48000 Hz), for the mixer
// (sys/mixer.h) to play. *frames is a new buffer, free() it when done; *count is how many
// stereo frames it holds. Returns SOUND_OK or a SOUND_ERR_ code
int sound_load (const char* path, int16_t** frames, uint32_t* count);

// stop the startup sound if it is still playing, and free its memory
void sound_stop (void);

const char* sound_strerror (int error);

#endif /* SOUND_H */

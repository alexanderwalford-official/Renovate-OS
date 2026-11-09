/*
    mixer.c - several sounds at once (see mixer.h for the overview)

    TWO SIDES SHARE THE VOICES

    The program side (mixer_play, mixer_stop...) starts and stops voices. The interrupt side
    (mix, called by the sound card driver from the timer interrupt) reads the voices to make each
    block of sound, and marks a voice finished when its sound runs out. The timer interrupt can
    arrive at any moment, even halfway through mixer_play setting a voice up, so the program side
    turns interrupts off for the few instructions it takes to change a voice, and back on after.

    VOICE NUMBERS

    A voice number is the voice's slot (0 to 7) plus 8 times a "generation" that counts up each
    time the slot is reused. So if a sound ends and its slot starts another one, the old number
    no longer matches, and mixer_stop() on it can't stop the wrong sound by mistake.
*/

#include "mixer.h"
#include "sound.h"
#include "sys_clck.h"
#include "../drivers/audio/ac97.h"
#include "../drivers/input/GIO.h"
#include "../lib/aer_string.h"

typedef struct {
    bool active;
    const int16_t* frames;  // the sound: left, right, left, right...
    uint32_t count;         // how many stereo frames it has
    uint32_t position;      // the next frame to play
    int volume;             // 0 to MIXER_FULL_VOLUME
    bool loop;              // start again from the beginning at the end
    uint32_t generation;    // counts up each time the slot is reused (see the top of the file)
} voice_t;

static voice_t voices[MIXER_VOICES];
static bool running = false; // has the mixer got the sound card?

// the biggest block the sound card driver asks for (STREAM_BLOCK_FRAMES in ac97.c is 512)
#define MOST_FRAMES 1024
static int32_t sums[MOST_FRAMES * 2]; // the voices added up, before clipping to 16 bits

// ---- the interrupt side -----------------------------------------------------------------------

// add one voice into sums[], `count` frames of it
static void add_voice (voice_t* voice, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (voice->position >= voice->count) {
            if (!voice->loop) {
                voice->active = false; // played to the end
                return;
            }
            voice->position = 0;
        }
        const int16_t* frame = voice->frames + voice->position * 2;
        // multiply by the volume, then divide by 256 (>> 8): full volume leaves it as it was
        sums[i * 2] += (frame[0] * voice->volume) >> 8;
        sums[i * 2 + 1] += (frame[1] * voice->volume) >> 8;
        voice->position++;
    }
}

// a sample that might be too loud for 16 bits, cut down to fit. Two loud sounds added together
// can go past what 16 bits hold; cutting them off ("clipping") sounds harsh but is far better
// than letting the number wrap round, which sounds like a loud crack
static int16_t clip (int32_t sample) {
    if (sample > 32767) {
        return 32767;
    }
    if (sample < -32768) {
        return -32768;
    }
    return (int16_t)sample;
}

// the sound card driver's fill function: the next `count` frames of every voice, added up
static void mix (int16_t* out, uint32_t count) {
    if (count > MOST_FRAMES) {
        count = MOST_FRAMES;
    }
    memset(sums, 0, count * 2 * sizeof(sums[0]));
    for (int v = 0; v < MIXER_VOICES; v++) {
        if (voices[v].active) {
            add_voice(&voices[v], count);
        }
    }
    for (uint32_t i = 0; i < count * 2; i++) {
        out[i] = clip(sums[i]);
    }
}

// the timer interrupt calls this every millisecond; every other time, top up the sound card
static void tick (void) {
    static uint32_t counter = 0;
    counter++;
    if (counter % 2 == 0) {
        ac97_stream_service();
    }
}

// ---- the program side -------------------------------------------------------------------------

// turn interrupts off, saying whether they were on, so voices can be changed safely
static bool hold_interrupts (void) {
    bool were_on = interrupts_enabled();
    interrupts_disable();
    return were_on;
}

static void release_interrupts (bool were_on) {
    if (were_on) {
        interrupts_enable();
    }
}

// get the sound card, if the mixer hasn't got it already (or something else took it back)
static bool take_card (void) {
    if (running && ac97_streaming()) {
        return true;
    }
    sound_stop(); // the startup sound, if it is still playing
    if (!ac97_stream_start(mix)) {
        return false;
    }
    clck_tick_hook = tick;
    running = true;
    return true;
}

int mixer_play (const int16_t* frames, uint32_t count, bool loop, int volume) {
    if (frames == NULL || count == 0 || !take_card()) {
        return -1;
    }
    if (volume < 0) {
        volume = 0;
    }
    if (volume > MIXER_FULL_VOLUME) {
        volume = MIXER_FULL_VOLUME;
    }

    bool were_on = hold_interrupts();
    int number = -1;
    for (int slot = 0; slot < MIXER_VOICES; slot++) {
        voice_t* voice = &voices[slot];
        if (voice->active) {
            continue;
        }
        voice->frames = frames;
        voice->count = count;
        voice->position = 0;
        voice->volume = volume;
        voice->loop = loop;
        voice->generation++;
        voice->active = true; // last, so the interrupt never sees a half set up voice
        number = slot + MIXER_VOICES * (int)(voice->generation % 1000000);
        break;
    }
    release_interrupts(were_on);
    return number;
}

// the voice a number stands for, NULL if it is out of date or not a voice at all
static voice_t* find_voice (int number) {
    if (number < 0) {
        return NULL;
    }
    int slot = number % MIXER_VOICES;
    uint32_t generation = (uint32_t)(number / MIXER_VOICES);
    voice_t* voice = &voices[slot];
    if (voice->generation % 1000000 != generation) {
        return NULL;
    }
    return voice;
}

void mixer_stop (int number) {
    bool were_on = hold_interrupts();
    voice_t* voice = find_voice(number);
    if (voice != NULL) {
        voice->active = false;
    }
    release_interrupts(were_on);
}

bool mixer_playing (int number) {
    voice_t* voice = find_voice(number);
    return voice != NULL && voice->active;
}

void mixer_stop_all (void) {
    bool were_on = hold_interrupts();
    for (int slot = 0; slot < MIXER_VOICES; slot++) {
        voices[slot].active = false;
    }
    release_interrupts(were_on);

    if (running) {
        clck_tick_hook = NULL;
        ac97_stream_stop();
        running = false;
    }
}

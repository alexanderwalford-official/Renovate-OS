/*
    mixer.c - several sounds at once (see mixer.h for the overview)

    TWO SIDES SHARE THE VOICES

    The program side (mixer_play, mixer_stop...) starts and stops voices. The interrupt side
    (mix, called by the sound card driver from the timer interrupt) reads the voices to make each
    block of sound, and marks a voice finished when its sound runs out. The timer interrupt can
    arrive at any moment, even halfway through mixer_play setting a voice up, so the program side
    turns interrupts off for the few instructions it takes to change a voice, and back on after.

    TWO KINDS OF VOICE

        a sound    mixer_play(): a whole sound, already in memory, played once or looped
        a stream   mixer_stream_open(): sound that is made while it plays (a video's sound track).
                   The voice has a ring buffer: the program writes to it with mixer_stream_write,
                   the interrupt reads from it, and if the program falls behind the voice plays
                   silence until more arrives

    The ring buffer is shared without locks, because each side only ever moves its own counter:

        written   frames the program has put in (only the program side changes it)
        read      frames the interrupt has taken out (only the interrupt side changes it)

        written - read   = frames waiting to be played
        counter % size   = where in the buffer that frame is (size is a power of two, so this is
                           just counter & (size - 1))

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
#include "../lib/aer_stdlib.h"

typedef struct {
    bool active;
    bool paused;            // keeps its place but plays nothing
    int volume;             // 0 to MIXER_FULL_VOLUME
    uint32_t generation;    // counts up each time the slot is reused (see the top of the file)
    volatile uint32_t played; // frames played so far (for mixer_played)

    // a sound (mixer_play)
    const int16_t* frames;  // the sound: left, right, left, right...
    uint32_t count;         // how many stereo frames it has
    uint32_t position;      // the next frame to play
    bool loop;              // start again from the beginning at the end

    // a stream (mixer_stream_open)
    bool stream;
    int16_t* ring;          // the ring buffer, ring_frames stereo frames
    uint32_t ring_frames;   // always a power of two
    volatile uint32_t written;
    volatile uint32_t read;
} voice_t;

static voice_t voices[MIXER_VOICES];
static bool running = false; // has the mixer got the sound card?

// the biggest block the sound card driver asks for (STREAM_BLOCK_FRAMES in ac97.c is 512)
#define MOST_FRAMES 1024
static int32_t sums[MOST_FRAMES * 2]; // the voices added up, before clipping to 16 bits

// ---- the interrupt side -----------------------------------------------------------------------

// add one stereo frame to sums[] at position i, at the voice's volume. Multiplying by the
// volume and dividing by 256 (>> 8) leaves it as it was at full volume
static void add_frame (uint32_t i, const int16_t* frame, int volume) {
    sums[i * 2] += (frame[0] * volume) >> 8;
    sums[i * 2 + 1] += (frame[1] * volume) >> 8;
}

// add `count` frames of a sound voice into sums[]
static void add_sound (voice_t* voice, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (voice->position >= voice->count) {
            if (!voice->loop) {
                voice->active = false; // played to the end
                return;
            }
            voice->position = 0;
        }
        add_frame(i, voice->frames + voice->position * 2, voice->volume);
        voice->position++;
        voice->played++;
    }
}

// add up to `count` frames of a stream voice into sums[]: whatever is waiting in its ring
// buffer. If there isn't enough, the rest of the block is silence from this voice
static void add_stream (voice_t* voice, uint32_t count) {
    uint32_t waiting = voice->written - voice->read;
    if (count > waiting) {
        count = waiting;
    }
    uint32_t mask = voice->ring_frames - 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = (voice->read + i) & mask;
        add_frame(i, voice->ring + slot * 2, voice->volume);
    }
    voice->read += count;
    voice->played += count;
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
        voice_t* voice = &voices[v];
        if (!voice->active || voice->paused) {
            continue;
        }
        if (voice->stream) {
            add_stream(voice, count);
        } else {
            add_sound(voice, count);
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

static int clamp_volume (int volume) {
    if (volume < 0) {
        return 0;
    }
    if (volume > MIXER_FULL_VOLUME) {
        return MIXER_FULL_VOLUME;
    }
    return volume;
}

// find a free slot and start setting it up (interrupts must be off). NULL if all are busy
static voice_t* claim_slot (int* number) {
    for (int slot = 0; slot < MIXER_VOICES; slot++) {
        voice_t* voice = &voices[slot];
        if (voice->active) {
            continue;
        }
        uint32_t generation = voice->generation + 1;
        memset(voice, 0, sizeof(*voice));
        voice->generation = generation;
        *number = slot + MIXER_VOICES * (int)(generation % 1000000);
        return voice;
    }
    return NULL;
}

int mixer_play (const int16_t* frames, uint32_t count, bool loop, int volume) {
    if (frames == NULL || count == 0 || !take_card()) {
        return -1;
    }
    bool were_on = hold_interrupts();
    int number = -1;
    voice_t* voice = claim_slot(&number);
    if (voice != NULL) {
        voice->frames = frames;
        voice->count = count;
        voice->volume = clamp_volume(volume);
        voice->loop = loop;
        voice->active = true; // last, so the interrupt never sees a half set up voice
    }
    release_interrupts(were_on);
    return number;
}

int mixer_stream_open (uint32_t buffer_frames, int volume) {
    // round the buffer up to a power of two (see the top of the file)
    uint32_t frames = 1024;
    while (frames < buffer_frames && frames < (1u << 22)) {
        frames *= 2;
    }
    int16_t* ring = malloc(frames * 2 * sizeof(int16_t));
    if (ring == NULL) {
        return -1;
    }
    if (!take_card()) {
        free(ring);
        return -1;
    }

    bool were_on = hold_interrupts();
    int number = -1;
    voice_t* voice = claim_slot(&number);
    if (voice != NULL) {
        voice->stream = true;
        voice->ring = ring;
        voice->ring_frames = frames;
        voice->volume = clamp_volume(volume);
        voice->active = true;
    }
    release_interrupts(were_on);

    if (voice == NULL) {
        free(ring);
    }
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

uint32_t mixer_stream_room (int number) {
    voice_t* voice = find_voice(number);
    if (voice == NULL || !voice->stream || !voice->active) {
        return 0;
    }
    return voice->ring_frames - (voice->written - voice->read);
}

uint32_t mixer_stream_queued (int number) {
    voice_t* voice = find_voice(number);
    if (voice == NULL || !voice->stream || !voice->active) {
        return 0;
    }
    return voice->written - voice->read;
}

uint32_t mixer_stream_write (int number, const int16_t* frames, uint32_t count) {
    uint32_t room = mixer_stream_room(number);
    if (count > room) {
        count = room;
    }
    voice_t* voice = find_voice(number);
    if (voice == NULL || count == 0) {
        return 0;
    }
    uint32_t mask = voice->ring_frames - 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = (voice->written + i) & mask;
        voice->ring[slot * 2] = frames[i * 2];
        voice->ring[slot * 2 + 1] = frames[i * 2 + 1];
    }
    // only now move the counter, so the interrupt never reads frames before they are there
    voice->written += count;
    return count;
}

void mixer_stream_clear (int number) {
    bool were_on = hold_interrupts();
    voice_t* voice = find_voice(number);
    if (voice != NULL && voice->stream) {
        voice->read = voice->written; // nothing left waiting
        voice->played = 0;
    }
    release_interrupts(were_on);
}

void mixer_pause (int number, bool paused) {
    bool were_on = hold_interrupts();
    voice_t* voice = find_voice(number);
    if (voice != NULL) {
        voice->paused = paused;
    }
    release_interrupts(were_on);
}

uint32_t mixer_played (int number) {
    voice_t* voice = find_voice(number);
    if (voice == NULL) {
        return 0;
    }
    return voice->played;
}

uint32_t mixer_delay (void) {
    return ac97_stream_delay();
}

// stop a voice and give back its ring buffer (the voice is off before the buffer goes, so the
// interrupt can't be reading it)
static void stop_voice (voice_t* voice) {
    bool were_on = hold_interrupts();
    voice->active = false;
    int16_t* ring = voice->ring;
    voice->ring = NULL;
    release_interrupts(were_on);
    free(ring);
}

void mixer_stop (int number) {
    voice_t* voice = find_voice(number);
    if (voice != NULL) {
        stop_voice(voice);
    }
}

bool mixer_playing (int number) {
    voice_t* voice = find_voice(number);
    return voice != NULL && voice->active;
}

void mixer_stop_all (void) {
    for (int slot = 0; slot < MIXER_VOICES; slot++) {
        stop_voice(&voices[slot]);
    }
    if (running) {
        clck_tick_hook = NULL;
        ac97_stream_stop();
        running = false;
    }
}

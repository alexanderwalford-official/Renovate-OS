/*
    sound.c - playing WAV and MP3 files (see sound.h)

    A WAV file is a "RIFF" file: a 12 byte header ("RIFF", size, "WAVE") followed by chunks, each
    a 4 letter name, a 32 bit size and the data (padded to an even length). Two chunks matter:

        "fmt "  format (1 = PCM, 0xFFFE = "extensible", which is PCM with extra details),
                channels, sample rate, bytes per frame and bits per sample
        "data"  the samples, frame by frame (left then right for stereo)

    8 bit samples are unsigned (128 = silence), wider ones are signed. Everything is turned into
    16 bit by keeping the top 16 bits of each sample.

    The path every sound takes:

        file bytes --+-- "RIFF....WAVE" --> wav_decode --+
                     |                                    +--> 16 bit stereo (pcm_t)
                     +-- ID3 / MPEG frame --> mp3_decode -+         |
                                                                    v
                                     the card's rate? -- no --> resample (straight lines)
                                                                    |
                                                                    v
                                      PLY / play(): ac97_play (waits until the end)
                                      startup sound: ac97_start (plays in the background)
*/

#include "sound.h"
#include <stdint.h>
#include "../lib/primary_definitions.h"
#include "../lib/mp3.h"
#include "../drivers/audio/ac97.h"
#include "f_sys.h"
#include "apps.h"

// decoded sound: count stereo frames of 16 bit samples at rate Hz
typedef struct {
    int16_t* frames;
    uint32_t count;
    uint32_t rate;
} pcm_t;

// a 16 bit number from a WAV file. WAV files are little endian (lowest byte first). Reading it a
// byte at a time also works when p isn't on an even address, which chunks often aren't
static uint16_t read_u16 (const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

// a 32 bit little endian number, built the same way as read_u16
static uint32_t read_u32 (const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// whether a file looks like a WAV: "RIFF" at the start and "WAVE" after the 4 byte size
static bool wav_detect (const uint8_t* data, uint32_t size) {
    return size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVE", 4) == 0;
}

// one sample as a 16 bit value, whatever its size in the file.
// 8 bit: unsigned, so take 128 away to centre it on 0, then shift it up to fill 16 bits.
// 24 and 32 bit: the samples are little endian, so the top 16 bits are the last two bytes, and
// the lower bytes (detail too quiet to hear in 16 bits) are simply dropped
static int16_t wav_sample (const uint8_t* p, uint16_t bits) {
    switch (bits) {
        case 8:  return (int16_t)((p[0] - 128) << 8);
        case 16: return (int16_t)read_u16(p);
        case 24: return (int16_t)read_u16(p + 1);
        default: return (int16_t)read_u16(p + 2); // 32
    }
}

// find the "fmt " and "data" chunks (other chunks, like "LIST" with the title, are skipped)
// and convert every frame to 16 bit stereo
static int wav_decode (const uint8_t* data, uint32_t size, pcm_t* out) {
    uint16_t channels = 0, bits = 0, frame_bytes = 0;
    const uint8_t* samples = 0;
    uint32_t samples_size = 0;

    // walk the chunks after the 12 byte header. Each chunk is: 4 letter name, 32 bit length,
    // then `length` bytes of body. The loop stops once there isn't room left for a chunk header
    for (uint32_t at = 12; at + 8 <= size;) {
        uint32_t length = read_u32(data + at + 4);
        const uint8_t* body = data + at + 8;
        if (length > size - at - 8) {
            length = size - at - 8; // a cut short file, use what's there
        }
        // the format chunk is at least 16 bytes. Its fields, from the start of the body:
        //     0  format (2 bytes)        2  channels (2)
        //     4  sample rate (4)         8  bytes per second (4, not needed)
        //    12  bytes per frame (2)    14  bits per sample (2)
        if (memcmp(data + at, "fmt ", 4) == 0 && length >= 16) {
            uint16_t format = read_u16(body);
            // extensible: the real format is the first 2 bytes of the sub format at offset 24
            if (format == 0xFFFE && length >= 26) {
                format = read_u16(body + 24);
            }
            if (format != 1) {
                return SOUND_ERR_FORMAT;
            }
            channels = read_u16(body + 2);
            out->rate = read_u32(body + 4);
            frame_bytes = read_u16(body + 12);
            bits = read_u16(body + 14);
        } else if (memcmp(data + at, "data", 4) == 0) {
            samples = body;
            samples_size = length;
        }
        // on to the next chunk: past the header, the body, and a pad byte if the length is odd
        at += 8 + length + (length & 1);
    }
    // both chunks must have been found, with sizes that make sense. frame_bytes has to hold
    // every channel's sample, or reading a frame would run into the next one (and 0 would
    // divide by zero below)
    if (samples == 0 || channels == 0 || out->rate == 0 ||
        (bits != 8 && bits != 16 && bits != 24 && bits != 32) || frame_bytes < channels * (bits / 8)) {
        return SOUND_ERR_FORMAT;
    }

    // 4 bytes per output frame (two 16 bit samples). The extra 4 keeps it from being a malloc
    // of 0 when the data chunk is empty
    out->count = samples_size / frame_bytes;
    out->frames = malloc((size_t)out->count * 4 + 4);
    if (out->frames == 0) {
        return SOUND_ERR_NO_MEMORY;
    }
    // frame_bytes is used to step from frame to frame rather than channels * sample_bytes, as
    // the file says how far apart they are (it can be more, e.g. 24 bit samples padded to 32)
    uint32_t sample_bytes = bits / 8;
    for (uint32_t i = 0; i < out->count; i++) {
        const uint8_t* frame = samples + i * frame_bytes;
        int16_t left = wav_sample(frame, bits);
        // mono plays the same on both sides, anything past stereo is left out
        out->frames[i * 2] = left;
        out->frames[i * 2 + 1] = channels > 1 ? wav_sample(frame + sample_bytes, bits) : left;
    }
    return SOUND_OK;
}

// change the sample rate by drawing a straight line between neighbouring samples
// (simple, and good enough for the usual 44100 -> 48000). For each output frame:
//
//     position in the input = output frame * (old rate / new rate)     e.g. 7.25
//     sample = input[7] + 0.25 * (input[8] - input[7])
//
// all in whole numbers: the position is 16.16 fixed point (the top part is the frame, the
// bottom 16 bits the fraction), so stepping along is one addition per frame
static int resample (pcm_t* pcm, uint32_t rate) {
    // step through the input in 16.16 fixed point (the position is 64 bit so long sounds don't
    // overflow). The step is worked out in two halves to avoid a 64 bit division, which the
    // kernel can't do: the remainder is below rate (at most 48000), so shifting it 16 still fits
    uint32_t step = ((pcm->rate / rate) << 16) + ((pcm->rate % rate) << 16) / rate;
    // at most count / old rate * new rate frames, plus a second for the rounding
    uint32_t most = pcm->count / pcm->rate * rate + rate;
    int16_t* out = malloc((size_t)most * 4);
    if (out == 0) {
        return SOUND_ERR_NO_MEMORY;
    }

    // one output frame per time round, until the input runs out (or, just in case, `out` is full)
    uint32_t made = 0;
    for (uint64_t at = 0; (uint32_t)(at >> 16) < pcm->count && made < most; at += step, made++) {
        // the input frame we are at, and the one after it (the last frame has no next, so it
        // pairs with itself and is just copied)
        uint32_t i = (uint32_t)(at >> 16);
        uint32_t next = i + 1 < pcm->count ? i + 1 : i;
        // how far between the two we are: 0 = right on frame i, 32767 = nearly at the next.
        // (b - a) can be up to 65535, and that times 15 bits still fits in a signed 32 bit number
        int32_t fraction = (int32_t)((at & 0xFFFF) >> 1); // 15 bits, so the multiply fits in 32
        // the same straight line, once for the left sample and once for the right
        for (int side = 0; side < 2; side++) {
            int32_t a = pcm->frames[i * 2 + side];
            int32_t b = pcm->frames[next * 2 + side];
            out[made * 2 + side] = (int16_t)(a + (((b - a) * fraction) >> 15));
        }
    }
    // swap the old frames for the new ones
    free(pcm->frames);
    pcm->frames = out;
    pcm->count = made;
    pcm->rate = rate;
    return SOUND_OK;
}

// a WAV or MP3 file's contents -> 16 bit stereo.
// rate = the rate it has to end up at, or 0 for "whatever the card can play": the card is then
// asked for the sound's own rate, and the sound converted only if the card can't do that one
static int decode (const uint8_t* data, uint32_t size, pcm_t* pcm, uint32_t rate) {
    // pick the decoder by what the start of the file looks like (its name is never looked at).
    // Anything that is neither stays SOUND_ERR_FORMAT
    int result = SOUND_ERR_FORMAT;
    if (wav_detect(data, size)) {
        result = wav_decode(data, size, pcm);
    } else if (mp3_detect(data, size)) {
        pcm->frames = mp3_decode(data, size, &pcm->count, &pcm->rate);
        result = pcm->frames ? SOUND_OK : SOUND_ERR_FORMAT;
    }
    if (result != SOUND_OK) {
        return result;
    }
    // ac97_set_rate answers with the rate the card really ended up at (48000 if it can only
    // do that one), which is the rate the sound has to be in
    if (rate == 0) {
        rate = ac97_set_rate(pcm->rate);
    }
    // resampling costs time and a second copy of the sound, so only when it is needed
    if (rate != pcm->rate) {
        result = resample(pcm, rate);
    }
    return result;
}

// the sound playing in the background (the startup sound), kept until it is stopped
// (the card reads it straight from memory by DMA, so it can't be freed while it plays)
static int16_t* background = 0;

// stop the background sound and only then free it, so the card is never left reading freed memory
static void stop_background (void) {
    ac97_stop();
    free(background);
    background = 0;
}

// play a WAV or MP3 file to the end (or until should_stop says so), see sound.h
int sound_play_file (const char* path, bool (*should_stop)(void)) {
    if (!ac97_ready) {
        return SOUND_ERR_NO_CARD;
    }
    // the file system wants the folder and the file name separately
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    if (!fs_split_path(path, dir, sizeof(dir), name, sizeof(name)) || !fs_file_exists(dir, name)) {
        return SOUND_ERR_NOT_FOUND;
    }
    uint32_t size = 0;
    uint8_t* data = (uint8_t*)fs_read_bytes(dir, name, &size);
    if (data == 0) {
        return SOUND_ERR_NO_MEMORY;
    }

    stop_background(); // one sound at a time
    // decode at whatever rate the card can do. The file itself isn't needed once it is decoded,
    // so it goes straight away to leave more memory while the sound plays
    pcm_t pcm = { 0, 0, 0 };
    int result = decode(data, size, &pcm, 0);
    free(data);
    // ac97_play only comes back when the sound has finished, been stopped or the card stalled
    if (result == SOUND_OK && !ac97_play(pcm.frames, pcm.count, should_stop)) {
        result = SOUND_ERR_CARD;
    }
    free(pcm.frames);
    return result;
}

// start the startup sound playing in the background, if there is a sound card
void sound_startup (void) {
    if (!ac97_init()) {
        return;
    }
    // the file system isn't set up yet (the boot options can still choose demo mode), so the
    // disk is read directly. No disk, or no file on it yet (the first boot), uses the built-in copy
    char status[64];
    uint32_t size = 0;
    uint8_t* file = 0;
    if (fat_mount_auto(status, sizeof(status))) {
        file = (uint8_t*)fat_read_file("/sys/", "startup.wav", &size);
    }
    const uint8_t* data = file ? file : apps_builtin("startup.wav", &size);

    // an empty (or any other unplayable) file just means no sound
    pcm_t pcm = { 0, 0, 0 };
    if (data && decode(data, size, &pcm, 0) == SOUND_OK) {
        // keep the frames (the card plays them from memory) and return while it plays
        stop_background();
        background = pcm.frames;
        ac97_start(pcm.frames, pcm.count);
    } else {
        free(pcm.frames);
    }
    // only the copy read from disk is freed: the built-in one is part of the kernel, not the heap
    free(file);
}

// stop the startup sound, if it is still playing
void sound_stop (void) {
    stop_background();
}

// decode a sound file into memory for the mixer, see sound.h
int sound_load (const char* path, int16_t** frames, uint32_t* count) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    if (!fs_split_path(path, dir, sizeof(dir), name, sizeof(name)) || !fs_file_exists(dir, name)) {
        return SOUND_ERR_NOT_FOUND;
    }
    uint32_t size = 0;
    uint8_t* data = (uint8_t*)fs_read_bytes(dir, name, &size);
    if (data == 0) {
        return SOUND_ERR_NO_MEMORY;
    }
    // always at the mixer's rate, without touching the card (it may be busy playing others)
    pcm_t pcm = { 0, 0, 0 };
    int result = decode(data, size, &pcm, AC97_STREAM_RATE);
    free(data);
    if (result != SOUND_OK) {
        free(pcm.frames);
        return result;
    }
    *frames = pcm.frames;
    *count = pcm.count;
    return SOUND_OK;
}

// a SOUND_ERR_ number in words, for error messages
const char* sound_strerror (int error) {
    switch (error) {
        case SOUND_OK: return "ok";
        case SOUND_ERR_NO_CARD: return "no sound card was found (AerOS supports AC'97)";
        case SOUND_ERR_NOT_FOUND: return "no such file";
        case SOUND_ERR_FORMAT: return "not a WAV or MP3 file AerOS can play";
        case SOUND_ERR_NO_MEMORY: return "not enough memory";
        default: return "the sound card stopped responding";
    }
}

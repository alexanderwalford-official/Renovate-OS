/*
    mp3.c - MP3 decoding with minimp3 (see mp3.h)

    minimp3 does its maths with floats. The kernel is built without SSE, so gcc uses the x87 FPU
    for them, which has to be switched on once before the first float instruction (fpu_enable).
*/

#include "mp3.h"
#include "aer_stdlib.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD // plain C only, SSE isn't turned on in AerOS
#include "minimp3/minimp3.h"

// turn on the x87 FPU: CR0 bit 2 (EM, "no FPU, trap every float instruction") and bit 3 (TS)
// off, bit 1 (MP) and bit 5 (NE, report errors as exceptions) on, then reset the FPU itself
static void fpu_enable (void) {
    static bool enabled = false;
    if (enabled) return;
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1u << 2) | (1u << 3));
    cr0 |= (1u << 1) | (1u << 5);
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile ("fninit");
    enabled = true;
}

// an ID3v2 tag (title, artist, cover art...) can sit before the first frame, skip over it
// its size is stored in 4 bytes of 7 bits each, plus 10 for the header and 10 more for a footer
static uint32_t id3_size (const uint8_t* data, uint32_t size) {
    if (size < 10 || memcmp(data, "ID3", 3) != 0) return 0;
    uint32_t tag = ((uint32_t)(data[6] & 0x7F) << 21) | ((uint32_t)(data[7] & 0x7F) << 14) |
                   ((uint32_t)(data[8] & 0x7F) << 7) | (data[9] & 0x7F);
    tag += (data[5] & 0x10) ? 20 : 10;
    return tag < size ? tag : size;
}

bool mp3_detect (const uint8_t* data, uint32_t size) {
    if (size >= 3 && memcmp(data, "ID3", 3) == 0) return true;
    // an MPEG audio frame starts with 11 set bits
    return size >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0;
}

int16_t* mp3_decode (const uint8_t* data, uint32_t size, uint32_t* frames, uint32_t* rate) {
    fpu_enable();
    uint32_t skip = id3_size(data, size);
    data += skip;
    size -= skip;

    static mp3dec_t decoder; // about 6KB, too big for the kernel stack
    mp3dec_frame_info_t info;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_init(&decoder);

    int16_t* out = 0;
    uint32_t count = 0, capacity = 0;
    *rate = 0;
    while (size > 0) {
        int samples = mp3dec_decode_frame(&decoder, data, (int)size, pcm, &info);
        if (info.frame_bytes == 0) break; // no more frames in what's left
        data += info.frame_bytes;
        size -= (uint32_t)info.frame_bytes;
        if (samples == 0) continue; // it skipped something that wasn't audio

        if (*rate == 0) *rate = (uint32_t)info.hz;
        if (count + (uint32_t)samples > capacity) {
            // grow by doubling, starting at about a second
            capacity = capacity ? capacity * 2 : 48000;
            int16_t* bigger = realloc(out, capacity * 2 * sizeof(int16_t));
            if (bigger == 0) {
                free(out);
                return 0;
            }
            out = bigger;
        }
        for (int i = 0; i < samples; i++) {
            int16_t left = pcm[i * info.channels];
            out[(count + i) * 2] = left;
            out[(count + i) * 2 + 1] = info.channels == 2 ? pcm[i * 2 + 1] : left;
        }
        count += (uint32_t)samples;
    }
    if (count == 0) {
        free(out);
        return 0;
    }
    *frames = count;
    return out;
}

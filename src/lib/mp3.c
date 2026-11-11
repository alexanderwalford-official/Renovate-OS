/*
    mp3.c - MP3 decoding with minimp3 (see mp3.h)

    minimp3 does its maths with floats, which need the x87 FPU switched on first (see fpu.h).
*/

#include "mp3.h"
#include "aer_stdlib.h"
#include "busy.h"
#include "fpu.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD // plain C only, SSE isn't turned on in AerOS
#include "minimp3/minimp3.h"

// an ID3v2 tag (title, artist, cover art...) can sit before the first frame, skip over it
// its size is stored in 4 bytes of 7 bits each, plus 10 for the header and 10 more for a footer
static uint32_t id3_size (const uint8_t* data, uint32_t size) {
    if (size < 10 || memcmp(data, "ID3", 3) != 0) {
        return 0;
    }
    // bytes 6 to 9: the size, most significant first. Only 7 bits of each byte are used (the top
    // bit is always 0) so the size can never look like the 0xFF that starts an audio frame
    uint32_t tag = ((uint32_t)(data[6] & 0x7F) << 21) | ((uint32_t)(data[7] & 0x7F) << 14) |
                   ((uint32_t)(data[8] & 0x7F) << 7) | (data[9] & 0x7F);
    // byte 5 is the flags, and 0x10 means a 10 byte footer follows the tag
    tag += (data[5] & 0x10) ? 20 : 10;
    // a broken size can't send us past the end of the file
    return tag < size ? tag : size;
}

// whether a file looks like an MP3: an ID3 tag, or straight into an audio frame
bool mp3_detect (const uint8_t* data, uint32_t size) {
    if (size >= 3 && memcmp(data, "ID3", 3) == 0) {
        return true;
    }
    // an MPEG audio frame starts with 11 set bits (all 8 of the first byte, the top 3 of the next)
    return size >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0;
}

// decode a whole MP3 into 16 bit stereo frames on the heap, see mp3.h. NULL if none were found
int16_t* mp3_decode (const uint8_t* data, uint32_t size, uint32_t* frames, uint32_t* rate) {
    fpu_enable();
    // start at the first frame, after any ID3 tag
    uint32_t skip = id3_size(data, size);
    data += skip;
    size -= skip;

    static mp3dec_t decoder; // about 6KB, too big for the kernel stack
    mp3dec_frame_info_t info;
    // one frame's samples at most (1152 per channel, interleaved), what minimp3 writes into
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_init(&decoder);

    // the output grows as frames are decoded: count frames so far, room for capacity
    int16_t* out = 0;
    uint32_t count = 0, capacity = 0;
    *rate = 0;
    // one MPEG frame per time round, until the data runs out
    while (size > 0) {
        busy(); // a spinner can turn while a long song decodes
        // samples comes back per channel (so it is the number of frames), and info says how
        // many bytes the frame took, its channels and its rate
        int samples = mp3dec_decode_frame(&decoder, data, (int)size, pcm, &info);
        if (info.frame_bytes == 0) {
            break; // no more frames in what's left
        }
        data += info.frame_bytes;
        size -= (uint32_t)info.frame_bytes;
        if (samples == 0) {
            continue; // it skipped something that wasn't audio
        }

        // the first frame's rate is taken for the whole song (they all match in a normal file)
        if (*rate == 0) {
            *rate = (uint32_t)info.hz;
        }
        if (count + (uint32_t)samples > capacity) {
            // grow by doubling, starting at about a second. Doubling keeps the copying realloc
            // does to a small multiple of the song's own size, however long it is
            capacity = capacity ? capacity * 2 : 48000;
            int16_t* bigger = realloc(out, capacity * 2 * sizeof(int16_t));
            if (bigger == 0) {
                free(out);
                return 0;
            }
            out = bigger;
        }
        // copy the frame out as stereo: a mono frame's one sample goes to both sides
        for (int i = 0; i < samples; i++) {
            int16_t left = pcm[i * info.channels];
            out[(count + i) * 2] = left;
            out[(count + i) * 2 + 1] = info.channels == 2 ? pcm[i * 2 + 1] : left;
        }
        count += (uint32_t)samples;
    }
    // it looked like an MP3 but held no sound at all
    if (count == 0) {
        free(out);
        return 0;
    }
    *frames = count;
    return out;
}

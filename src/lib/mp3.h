/*
    mp3.h
    MP3 decoding, using minimp3 (lib/minimp3, public domain: https://github.com/lieff/minimp3).

    MP3's patents ran out in 2017, so it is free to use.
*/
#ifndef MP3_H
#define MP3_H

#include <stdint.h>
#include <stdbool.h>

// does this look like an MP3 file? (an ID3 tag or an MPEG audio frame at the start)
bool mp3_detect (const uint8_t* data, uint32_t size);

// decode a whole MP3 file into 16 bit stereo frames (left, right, left, right...)
// returns a new buffer (free it when done) and sets *frames and *rate, NULL if it couldn't be decoded
int16_t* mp3_decode (const uint8_t* data, uint32_t size, uint32_t* frames, uint32_t* rate);

#endif /* MP3_H */

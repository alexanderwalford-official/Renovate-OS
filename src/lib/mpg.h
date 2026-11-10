/*
    mpg.h - MPEG-1 video (.mpg files), using pl_mpeg
    (lib/pl_mpeg, MIT licence: https://github.com/phoboslab/pl_mpeg)

    An .mpg file ("MPEG program stream") holds two streams woven together: MPEG-1 video and
    MP2 sound. MPEG-1's patents have all run out, so it is free to use, and ffmpeg makes it:

        ffmpeg -i in.mp4 -c:v mpeg1video -q:v 4 -c:a mp2 -f mpeg out.mpg

    pl_mpeg does all the decoding. Include this file rather than pl_mpeg.h itself: it sets
    pl_mpeg up for AerOS first (no C standard library file functions, which AerOS doesn't have).
    The player built on it is sys/video.c.

    The parts of pl_mpeg AerOS uses (the full list is at the top of lib/pl_mpeg/pl_mpeg.h):

        plm_buffer_create_with_callbacks()   read the file a piece at a time, through AerOS's
                                             own functions
        plm_create_with_buffer()             a decoder reading from that
        plm_get_width / height / framerate / samplerate / duration
        plm_decode_video()                   the next picture (as Y, Cb, Cr planes)
        plm_frame_to_bgra()                  a picture turned into 4 byte pixels
        plm_decode_audio()                   the next 1152 stereo samples (floats, -1 to 1)
        plm_seek()                           jump to a time
*/
#ifndef MPG_H
#define MPG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PLM_NO_STDIO // AerOS has no fopen / fread: files are read through callbacks instead
#include "pl_mpeg/pl_mpeg.h"

// does this look like an .mpg file? (an MPEG program stream starts with the bytes 00 00 01 BA)
bool mpg_detect (const uint8_t* data, uint32_t size);

// switch the FPU on (pl_mpeg's sound decoder uses floats): call before decoding anything
void mpg_init (void);

#endif /* MPG_H */

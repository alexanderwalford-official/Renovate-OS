/*
    mpg.c - builds pl_mpeg into AerOS (see mpg.h)

    pl_mpeg is a "single header" library: the whole decoder is in pl_mpeg.h, and exactly one C
    file defines PL_MPEG_IMPLEMENTATION before including it to get the code. This is that file.
    The Makefile compiles it with lib/shim on the include path, so pl_mpeg's <string.h> and
    <stdlib.h> are AerOS's own.
*/

#include "mpg.h"
#include "aer_stdlib.h"
#include "aer_string.h"
#include "fpu.h"

// pl_mpeg asks for memory through these, so it uses the kernel heap
#define PLM_MALLOC(size)          malloc(size)
#define PLM_REALLOC(pointer, size) realloc(pointer, size)
#define PLM_FREE(pointer)         free(pointer)

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg/pl_mpeg.h"

// whether a file looks like an MPEG-1 video (a program stream)
bool mpg_detect (const uint8_t* data, uint32_t size) {
    // the "pack" start code every program stream begins with: 00 00 01 is the prefix every MPEG
    // start code has, and BA says this one is a pack header
    return size >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01 && data[3] == 0xBA;
}

// get ready to decode video: pl_mpeg does its maths with floats, so the FPU has to be on
void mpg_init (void) {
    fpu_enable();
}

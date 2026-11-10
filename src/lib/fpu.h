/*
    fpu.h - switching on the maths co-processor (the x87 FPU)

    Decimal-point numbers (float, double) are worked out by the FPU, a part of the processor
    that starts out switched off. Any float instruction before it is switched on stops the
    computer with an error.

    The kernel is built without SSE (the newer way of doing float maths, which would need saving
    and restoring on every interrupt), so GCC uses the x87 FPU for floats and doubles. Code that
    uses them, like the MP3 and MPEG decoders, calls fpu_enable() first. Calling it again does
    nothing.
*/
#ifndef FPU_H
#define FPU_H

void fpu_enable (void);

#endif /* FPU_H */

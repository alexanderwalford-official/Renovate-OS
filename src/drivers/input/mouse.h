/*
    mouse.h - the PS/2 mouse, for AerLang programs

    Only programs use the mouse: the terminal never does. The mouse is switched on the first time
    a program asks about it (mouse(), mousemove() or mousedown() in lang/aer_builtins.c) and off
    again when the program ends, so with no program running AerOS works just as it always has.

    How a PS/2 mouse talks:
    - It sits on the 8042 controller's second port, next to the keyboard on the first. Its bytes
      arrive on the same data port (0x60) and raise IRQ12 instead of IRQ1. The status register
      says which port a byte came from (KBC_FROM_MOUSE in IO.h), so ps2_service() in IO.c reads
      every byte and hands mouse bytes to this driver.
    - Each time the mouse moves or a button changes it sends a 3 byte packet:

          byte 0   bit 0 left, 1 right, 2 middle button held
                   bit 3 always 1 (how the start of a packet is found again if a byte is lost)
                   bit 4 / 5 the sign of the x / y movement (the 9th bit, so -256 to 255)
                   bit 6 / 7 x / y overflow: it moved further than that, the movement is lost
          byte 1   x movement, right is positive
          byte 2   y movement, UP is positive (the screen counts down, so it is turned round)

    - The movement is in "counts", about 4 a millimetre at the default settings. There is no
      position: the driver adds the movements up itself, keeping the pointer inside the screen.
*/

#ifndef MOUSE_H
#define MOUSE_H

#include <stdbool.h>

// the buttons, as bits of the `buttons` mouse_position gives
#define MOUSE_LEFT   1
#define MOUSE_RIGHT  2
#define MOUSE_MIDDLE 4

// switch the mouse on (it is fine to call this again while it is on). False if there is no
// mouse; it is then only looked for once, so a program asking every frame doesn't slow down
bool mouse_start(void);

// switch the mouse off again, as if it had never been on (a program has ended)
void mouse_stop(void);

// is the mouse on right now?
bool mouse_ready(void);

// the pointer stays inside width x height pixels. This also puts it in the middle
void mouse_set_area(int width, int height);

// where the pointer is, in pixels from the top left, and which buttons are held (MOUSE_ bits)
void mouse_position(int* x, int* y, int* buttons);

// how far the mouse has moved since the last call, in counts (right and down are positive).
// Unlike the position it never stops at the edge of the screen, which is what turning a camera
// needs. The count starts again from 0 after each call
void mouse_motion(int* dx, int* dy);

#endif /* MOUSE_H */

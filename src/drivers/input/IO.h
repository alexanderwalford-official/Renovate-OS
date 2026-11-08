/*
    IO.h
    Alexander Walford 2023

    PS/2 keyboard driver.
    The BIOS keyboard service (int 0x16) only exists in real mode, so in protected mode the
    keyboard is read directly: every key press / release raises IRQ1 and the scancode is read
    from port 0x60, translated to a character and stored in a small input buffer.

    https://wiki.osdev.org/PS/2_Keyboard
    https://wiki.osdev.org/%228042%22_PS/2_Controller

    The pieces, from the hardware up:
    - The keyboard talks to the "8042" PS/2 controller on the motherboard (today a part of the
      chipset, or emulated by the BIOS for USB keyboards, which is why a USB keyboard still works).
    - Each key sends a scancode when pressed and another when released. The controller hands
      them to us through port 0x60 and raises IRQ1 for each byte.
    - The IRQ1 handler in IO.c turns scancodes into characters (tracking shift, ctrl and caps lock)
      and puts them in a ring buffer.
    - GetChar() takes keys back out of that buffer, and GetString() builds a whole line editor
      with backspace and command history on top of it. The terminal uses GetString for its prompt.
*/

#ifndef IO_H
#define IO_H

#include <stdbool.h>

// special (non-ASCII) keys returned by GetChar()
// they start at 0x80 so they can never be confused with a normal 7 bit ASCII character
#define KEY_ESCAPE 27
#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_HOME   0x84
#define KEY_END    0x85
#define KEY_DELETE 0x86

// true once init_keyboard() has run, GetChar() refuses to wait for keys before that
extern bool keyboard_ready;

// function to initialize the keyboard (requires idt_init() first)
// sets up the 8042 controller, installs the IRQ1 handler and unmasks IRQ1 at the PIC.
// keys only start arriving once the kernel enables interrupts with sti
void init_keyboard(void);

// returns true if a key is waiting in the input buffer
// never blocks, so a program can poll the keyboard while doing something else (e.g. an animation)
bool kb_haschar(void);

// returns the next pressed key, waiting for one if the buffer is empty
// (ASCII characters, or one of the KEY_ special values), 0 if there is no keyboard
// "no keyboard" also covers interrupts being disabled, since then no key could ever arrive
int GetChar(void);

// standard style getchar(), waits for the next printable / control character
// the special KEY_ values (arrows etc.) are skipped, EOF is returned if there is no keyboard
int getchar(void);

// get input string: reads a line from the keyboard into in (at most size - 1 characters),
// echoing it to the console. Supports backspace and the up / down arrows for previous lines.
// returns in. The finished line (without the newline) is added to the history unless it is empty.
// size must be at least 1
char* GetString(char* in, int size);

// parse either 1 or 0 to enable to disable input buffer for current string
// while disabled, key presses are thrown away instead of queued (modifier keys are still tracked)
void InputBufferChangeState(int state);

// throw away every key press that has not been read yet
// (and anything that arrives before the next GetString starts reading)
void ClearInputbuffer(void);

#endif /* IO_H */

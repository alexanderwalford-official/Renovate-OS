/*
    IO.h
    Alexander Walford 2023

    PS/2 keyboard driver.
    The BIOS keyboard service (int 0x16) only exists in real mode, so in protected mode the
    keyboard is read directly: every key press / release raises IRQ1 and the scancode is read
    from port 0x60, translated to a character and stored in a small input buffer.

    https://wiki.osdev.org/PS/2_Keyboard
    https://wiki.osdev.org/%228042%22_PS/2_Controller
*/

#ifndef IO_H
#define IO_H

#include <stdbool.h>

// special (non-ASCII) keys returned by GetChar()
#define KEY_ESCAPE 27
#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_HOME   0x84
#define KEY_END    0x85
#define KEY_DELETE 0x86

extern bool keyboard_ready;

// function to initialize the keyboard (requires idt_init() first)
void init_keyboard(void);

// returns true if a key is waiting in the input buffer
bool kb_haschar(void);

// returns the next pressed key, waiting for one if the buffer is empty
// (ASCII characters, or one of the KEY_ special values), 0 if there is no keyboard
int GetChar(void);

// standard style getchar(), waits for the next printable / control character
int getchar(void);

// get input string: reads a line from the keyboard into in (at most size - 1 characters),
// echoing it to the console. Supports backspace and the up / down arrows for previous lines.
char* GetString(char* in, int size);

// parse either 1 or 0 to enable to disable input buffer for current string
void InputBufferChangeState(int state);

// throw away every key press that has not been read yet
void ClearInputbuffer(void);

#endif /* IO_H */

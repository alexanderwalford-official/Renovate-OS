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
    - getk() takes keys back out of that buffer, and getln() builds a whole line editor
      with backspace and command history on top of it. The terminal uses getln for its prompt.
*/

#ifndef IO_H
#define IO_H

#include <stdbool.h>
#include <stdint.h>

// The 8042 PS/2 controller. The keyboard is on its first port and the mouse on its second, and
// both share these ports (0x64 is the status register on read and the command register on write)
#define KBC_DATA    0x60
#define KBC_STATUS  0x64
#define KBC_COMMAND 0x64

// controller status bits
#define KBC_OUTPUT_FULL 0x01 // a byte is waiting for us
#define KBC_INPUT_FULL  0x02 // the controller has not taken the last byte yet
#define KBC_FROM_MOUSE  0x20 // the waiting byte came from the second port (the mouse)

// send a byte to the controller once it can take one: port is KBC_COMMAND for a command or
// KBC_DATA for a command's data byte. False if it never became ready (no controller)
bool kbc_write(uint16_t port, uint8_t value);

// wait until the controller has a byte to read from KBC_DATA. False if none came in time
bool kbc_wait_read(void);

// read the controller's waiting byte and hand it on: to the keyboard, or to the mouse sink if it
// came from the second port. The keyboard's IRQ1 handler and the mouse's IRQ12 handler both use it
void ps2_service(void);

// where mouse bytes go (drivers/input/mouse.c sets it while the mouse is on, NULL turns it off).
// Mouse bytes are never treated as key presses, whether or not there is a sink
void kb_set_mouse_sink(void (*sink)(uint8_t byte));

// special (non-ASCII) keys returned by getk()
// they start at 0x80 so they can never be confused with a normal 7 bit ASCII character
#define KEY_ESCAPE 27
#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_HOME   0x84
#define KEY_END    0x85
#define KEY_DELETE 0x86
#define KEY_PGUP   0x87
#define KEY_PGDN   0x88
// these three only ever show up in kb_is_down(): on their own they don't type anything
#define KEY_SHIFT  0x89
#define KEY_CTRL   0x8A
#define KEY_ALT    0x8B

// true once init_keyboard() has run, getk() refuses to wait for keys before that
extern bool keyboard_ready;

// function to initialize the keyboard (requires idt_init() first)
// sets up the 8042 controller, installs the IRQ1 handler and unmasks IRQ1 at the PIC.
// keys only start arriving once the kernel enables interrupts with sti
void init_keyboard(void);

// is this key held down right now? For games, where several keys can be down at once and
// what matters is whether they still are. key is a lower or upper case letter, a digit or
// symbol on the key without shift ('1', ' ', '\n', 27 for Escape...), or one of the KEY_ values
// above (arrows, KEY_SHIFT, KEY_CTRL, KEY_ALT)
bool kb_is_down(int key);

// returns true if a key is waiting in the input buffer
// never blocks, so a program can poll the keyboard while doing something else (e.g. an animation)
bool kb_haschar(void);

// true (once) if ctrl + C was pressed since the last call, used to stop a running program
bool kb_take_break(void);

// returns the next pressed key, waiting for one if the buffer is empty
// (ASCII characters, or one of the KEY_ special values), 0 if there is no keyboard
// "no keyboard" also covers interrupts being disabled, since then no key could ever arrive
int getk(void);

// standard style getchar(), waits for the next printable / control character
// the special KEY_ values (arrows etc.) are skipped, EOF is returned if there is no keyboard
int getchar(void);

// get input string: reads a line from the keyboard into in (at most size - 1 characters),
// echoing it to the console. Supports backspace and the up / down arrows for previous lines.
// returns in. The finished line (without the newline) is added to the history unless it is empty.
// size must be at least 1
char* getln(char* in, int size);

// what Tab does in GetLine. It is given the line typed so far (length characters, in a buffer of
// size bytes), adds to it in place, prints whatever it added (or a list of choices followed by
// the prompt and the line again), and returns the line's new length
typedef int (*tab_complete_fn)(char* line, int length, int size);

// getln, with Tab handed to complete (the terminal's command line uses it to fill in
// commands and file names). complete may be NULL, which is exactly getln
char* GetLine(char* in, int size, tab_complete_fn complete);

// forget the up / down arrow history (done on logout)
void ClearInputHistory(void);

// read a line without showing it (each character appears as *), for passwords
char* GetSecret(char* in, int size);

// parse either 1 or 0 to enable to disable input buffer for current string
// while disabled, key presses are thrown away instead of queued (modifier keys are still tracked)
void InputBufferChangeState(int state);

// throw away every key press that has not been read yet
// (and anything that arrives before the next getln starts reading)
void ClearInputbuffer(void);

#endif /* IO_H */

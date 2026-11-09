/*
    IO.c
    Alexander Walford 2023

    PS/2 keyboard driver: IRQ1 handler, scancode translation, input buffer and line editor.

    Scancodes
      The controller is told to "translate", so whatever the keyboard really speaks we receive
      scancode set 1, the original IBM PC XT codes. In set 1:
      - pressing a key sends its "make" code, e.g. 0x1E for A
      - releasing it sends the "break" code, which is the make code with bit 7 set (0x9E for A)
      - keys added after the original 83 key keyboard (arrow keys, right ctrl, keypad enter...)
        send the prefix byte 0xE0 first. For example 0x48 on its own is keypad 8, while
        0xE0 0x48 is the dedicated up arrow. Each byte raises its own IRQ1, so the prefix has to
        be remembered between interrupts (kb_extended).
      Scancodes say which physical key moved, not which character it means, so the driver keeps
      track of shift / ctrl / caps lock itself and uses lookup tables for a US QWERTY layout.

    The input buffer
      The IRQ handler (producer) and GetChar (consumer) share a ring buffer. The handler only ever
      moves kb_head and GetChar only ever moves kb_tail, so on a single CPU they can't tread on
      each other's toes and no lock is needed. The indices are volatile so GetChar's wait loop
      really re-reads kb_head after every interrupt instead of using a stale copy in a register.

    The 8042 controller
      Port 0x60 is the data port (scancodes in, data bytes for commands out), port 0x64 is the
      status register when read and the command register when written. Before writing anything
      we wait for the controller to empty its input buffer, and before reading we wait for its
      output buffer to fill, otherwise bytes get lost.

    https://wiki.osdev.org/PS/2_Keyboard#Scan_Code_Set_1
*/

#include "IO.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "GIO.h"
#include "../video/VGA_linear.h"
#include "../../lib/aer_ctype.h"
#include "../../lib/aer_string.h"
#include "../../lib/aer_stdio.h"
#include "../../sys/interrupts.h"
#include "../../lib/crypto.h"

// 8042 PS/2 controller ports (0x64 is the status register on read and the command register on write)
#define KBC_DATA    0x60
#define KBC_STATUS  0x64
#define KBC_COMMAND 0x64

// controller status bits
#define KBC_OUTPUT_FULL 0x01 // a byte is waiting for us
#define KBC_INPUT_FULL  0x02 // the controller has not taken the last byte yet

// number of slots in the key ring buffer, one is always left empty so it holds up to 127 keys
#define KB_BUFFER_SIZE 128

// the line editor remembers the last 8 lines, each up to 127 characters plus the NUL
#define INPUT_HISTORY_SIZE 8
#define INPUT_HISTORY_LEN  128

bool keyboard_ready = false;

static bool kb_input_enabled = false; // false = key presses are ignored
static bool kb_clear_pending = false; // set by ClearInputbuffer, handled by the next GetString

// ring buffer filled by the interrupt handler and emptied by GetChar()
// kb_head = the slot the next key is written to, kb_tail = the slot the next key is read from.
// head == tail means empty, and head one behind tail means full (that's why one slot stays unused:
// a completely full buffer would otherwise look exactly like an empty one)
static volatile uint8_t kb_buffer[KB_BUFFER_SIZE];
static volatile unsigned int kb_head = 0;
static volatile unsigned int kb_tail = 0;

// modifier key state
// shift and ctrl are true while the key is held down, caps lock flips on every press,
// kb_extended means the last byte was the 0xE0 prefix so the next one is an extended key
static bool kb_shift = false;
static bool kb_ctrl = false;
static bool kb_caps = false;
static bool kb_extended = false;
static volatile bool kb_break = false; // ctrl + C was pressed

// US QWERTY, scancode set 1 (index = scancode)
// written as a row of the keyboard per line. "\0" marks keys with no character (ctrl, shift, alt),
// the strings are glued together by the compiler, and the map covers scancodes 0x00 - 0x39.
// (0x37 is keypad *, 0x38 is left alt, 0x39 is the space bar)
// gotcha: the escape sits in its own string on purpose. Written as one string, "\x1B1" would be
// read as a single hex escape that swallows the 1 after it.
static const char kb_map_normal[] =
    "\0\x1B" "1234567890-=" "\b"  // 0x00 - 0x0E: nothing, escape, the number row, backspace
    "\t" "qwertyuiop[]" "\n"      // 0x0F - 0x1C: tab, the top letter row, enter
    "\0" "asdfghjkl;'`"           // 0x1D - 0x29: left ctrl, the home row, backtick
    "\0" "\\zxcvbnm,./" "\0"      // 0x2A - 0x36: left shift, backslash, the bottom row, right shift
    "*" "\0" " ";                 // 0x37 - 0x39: keypad *, left alt, space

// the same keys with shift held down
static const char kb_map_shift[] =
    "\0\x1B" "!@#$%^&*()_+" "\b"
    "\t" "QWERTYUIOP{}" "\n"
    "\0" "ASDFGHJKL:\"~"
    "\0" "|ZXCVBNM<>?" "\0"
    "*" "\0" " ";

// keypad keys 0x47 - 0x53
// num lock isn't tracked, so the keypad always types digits (the separate arrow / home / end keys
// send the same codes but are told apart by their 0xE0 prefix)
static const char kb_map_keypad[] = "789-456+1230.";

// command history for the line editor, used as a circular buffer of whole lines:
// once all 8 slots are used the newest line overwrites the oldest one
static char input_history[INPUT_HISTORY_SIZE][INPUT_HISTORY_LEN];
static int input_history_count = 0; // number of stored lines
static int input_history_next = 0;  // slot the next line goes into

// add a translated key to the ring buffer (called from the IRQ handler only)
// key: an ASCII character or one of the KEY_ values
// keys are silently dropped while input is disabled or when the buffer is full
static void kb_push(uint8_t key) {
    if (!kb_input_enabled) {
        return;
    }
    // work out where head moves to, wrapping round at the end of the array
    unsigned int next = (kb_head + 1) % KB_BUFFER_SIZE;
    if (next == kb_tail) {
        return; // buffer full, drop the key
    }
    // store the key first and only then publish it by moving head, so GetChar can never see a
    // slot that hasn't been filled in yet
    kb_buffer[kb_head] = key;
    kb_head = next;
}

// keys that follow the 0xE0 prefix byte
// code: the scancode with the release bit already stripped, released: true for a key release
static void kb_process_extended(uint8_t code, bool released) {
    // first the keys that matter on release too
    switch (code) {
        case 0x1D: kb_ctrl = !released; return; // right ctrl
        // some keyboards wrap keys like print screen or the arrows (with num lock on) in pretend
        // shift presses / releases, ignore them so they don't mess up kb_shift
        case 0x2A:
        case 0x36: return;                      // fake shifts sent with some extended keys
    }
    // everything else only does something when pressed
    if (released) {
        return;
    }
    // 0xE0 + a keypad code = the separate cursor keys, plus keypad enter and slash
    // (keys with no case here, like the Windows keys, are simply ignored)
    switch (code) {
        case 0x48: kb_push(KEY_UP); break;
        case 0x50: kb_push(KEY_DOWN); break;
        case 0x4B: kb_push(KEY_LEFT); break;
        case 0x4D: kb_push(KEY_RIGHT); break;
        case 0x47: kb_push(KEY_HOME); break;
        case 0x4F: kb_push(KEY_END); break;
        case 0x53: kb_push(KEY_DELETE); break;
        case 0x49: kb_push(KEY_PGUP); break;
        case 0x51: kb_push(KEY_PGDN); break;
        case 0x1C: kb_push('\n'); break; // keypad enter
        case 0x35: kb_push('/'); break;  // keypad slash
    }
}

// translate one scancode into a key and store it
// called once per byte the keyboard sends, so multi byte sequences arrive over several calls
static void kb_process_scancode(uint8_t scancode) {
    // the prefix on its own means nothing, remember it for the next byte
    if (scancode == 0xE0) {
        kb_extended = true;
        return;
    }

    // bit 7 set = break code (key released), the low 7 bits say which key
    bool released = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    if (kb_extended) {
        kb_extended = false;
        kb_process_extended(code, released);
        return;
    }

    // modifier keys: update our state, they never produce a character themselves
    switch (code) {
        case 0x2A:
        case 0x36: kb_shift = !released; return; // left / right shift
        case 0x1D: kb_ctrl = !released; return;  // left ctrl
        // caps lock toggles on the press only (the keyboard LED isn't updated)
        case 0x3A: if (!released) kb_caps = !kb_caps; return;
    }
    // releases of ordinary keys don't type anything
    if (released) {
        return;
    }

    // look the key up in the right table
    // sizeof includes the string's terminating NUL, hence the - 1
    char c = 0;
    if (code < sizeof(kb_map_normal) - 1) {
        c = kb_shift ? kb_map_shift[code] : kb_map_normal[code];
    } else if (code >= 0x47 && code <= 0x53) {
        c = kb_map_keypad[code - 0x47];
    }
    if (c == 0) {
        return; // function keys and other keys without a character
    }
    // caps lock only affects letters, and it flips their case, so shift + caps lock gives lower case
    if (kb_caps && isalpha(c)) {
        c = (char)(isupper(c) ? tolower(c) : toupper(c));
    }
    // ctrl + A is 1, ctrl + B is 2 ... the same control codes a real terminal sends
    // (so ctrl + H arrives as '\b' and ctrl + M as '\r', both handled by the line editor)
    if (kb_ctrl && isalpha(c)) {
        c = (char)(toupper(c) - 'A' + 1); // ctrl + letter = control code 1-26
    }
    // ctrl + C doesn't go in the buffer: it asks the running program to stop (see kb_take_break)
    if (c == 3) {
        kb_break = true;
        return;
    }
    kb_push((uint8_t)c);
}

bool kb_take_break(void) {
    bool pressed = kb_break;
    kb_break = false;
    return pressed;
}

// IRQ1 handler
// runs inside the interrupt with interrupts disabled, the generic IRQ code in sys/interrupts.c
// sends the end of interrupt to the PIC afterwards. Port 0x60 has to be read every time: until it
// is, the controller holds on to the byte and won't send us the next one.
static void keyboard_handler(struct regs* r) {
    (void)r;
    uint8_t scancode = inb(KBC_DATA);
    crypto_add_entropy(scancode); // key press timing feeds the random number generator
    kb_process_scancode(scancode);
}

// like GetString, but shows '*' for every character and keeps no history (for passwords)
char* GetSecret (char* in, int size) {
    int len = 0;
    for (;;) {
        int key = GetChar();
        if (key == 0 || key == '\n' || key == '\r') {
            break;
        }
        if (key == '\b') {
            if (len > 0) {
                len--;
                console_backspace();
            }
        } else if (isprint(key) && len < size - 1) {
            in[len++] = (char)key;
            console_putchar('*');
        }
    }
    in[len] = '\0';
    console_putchar('\n');
    return in;
}

// true if at least one key is waiting (the buffer is empty when head and tail meet)
bool kb_haschar(void) {
    return kb_head != kb_tail;
}

// take the oldest key out of the ring buffer, sleeping until one arrives if necessary
// returns the key (1-255), or 0 if no key can ever arrive
int GetChar(void) {
    if (!keyboard_ready || !interrupts_enabled()) {
        return 0; // no keyboard interrupts, waiting would hang forever
    }
    // a key could arrive just after the check and just before the hlt, in which case we sleep
    // through it. That's harmless here: the 1000 Hz timer interrupt wakes us within a millisecond
    // and the loop sees the key then.
    while (!kb_haschar()) {
        cpu_idle(); // sleep until the next interrupt
    }
    uint8_t key = kb_buffer[kb_tail];
    // read the key before moving tail, once tail moves the IRQ handler is free to reuse the slot
    kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;
    return key;
}

// getchar for programs that only want characters: the special KEY_ values (0x80 and up) are
// skipped, and EOF (-1) is returned when there is no keyboard
int getchar(void) {
    int key;
    do {
        key = GetChar();
        if (key == 0) {
            return EOF;
        }
    } while (key >= 0x80);
    return key;
}

// index 0 = oldest stored line
// the oldest line sits input_history_count slots behind input_history_next. INPUT_HISTORY_SIZE is
// added before the % because C's % keeps the sign, and next - count can be negative.
static const char* input_history_get(int index) {
    int slot = (input_history_next - input_history_count + index + INPUT_HISTORY_SIZE) % INPUT_HISTORY_SIZE;
    return input_history[slot];
}

// remember a finished line for the up / down arrows
// empty lines and a repeat of the previous line are skipped (pressing up should not show the same
// command several times in a row). Lines longer than 127 characters are cut short.
static void input_history_add(const char* line) {
    if (line[0] == '\0') {
        return;
    }
    if (input_history_count > 0 && strcmp(input_history_get(input_history_count - 1), line) == 0) {
        return; // same as the last line
    }
    strlcpy(input_history[input_history_next], line, INPUT_HISTORY_LEN);
    input_history_next = (input_history_next + 1) % INPUT_HISTORY_SIZE;
    // once the history is full count stays at 8 and each new line replaces the oldest one
    if (input_history_count < INPUT_HISTORY_SIZE) {
        input_history_count++;
    }
}

// forget every remembered line, so the next user to log in can't scroll back through them
void ClearInputHistory(void) {
    memset(input_history, 0, sizeof(input_history));
    input_history_count = 0;
    input_history_next = 0;
}

// the line editor: read keys until enter, echoing them and keeping in up to date as we go
// in: buffer that receives the line, size: its size in bytes (the line is at most size - 1 characters)
// returns in, which is left empty if there is no keyboard
// supported keys: printable characters (appended at the end), backspace, enter, up / down for the
// history, Page Up / Page Down to look back at earlier output, and Tab if a complete function
// was given. Anything else (left / right, escape, ctrl codes...) is ignored.
char* GetLine(char* in, int size, tab_complete_fn complete) {
    if (kb_clear_pending) {
        // throw away keys that were pressed between the clear and this point
        kb_tail = kb_head;
        kb_clear_pending = false;
    }

    int len = 0;
    int history_pos = input_history_count; // one past the newest entry = the line being typed
    in[0] = '\0';

    for (;;) {
        int key = GetChar();
        if (key == 0) {
            break; // no keyboard
        }
        // enter (or ctrl + M) finishes the line
        if (key == '\n' || key == '\r') {
            console_putchar('\n');
            break;
        }
        // backspace: drop the last character from the buffer and rub it out on screen
        if (key == '\b') {
            if (len > 0) {
                in[--len] = '\0';
                console_backspace();
            }
            continue;
        }
        // Page Up / Page Down look back through what has scrolled off the top of the screen, a
        // screen at a time (one line less, so the line at the edge stays in view). Any other key
        // comes back to the live screen first, so typing carries on where it was
        if (key == KEY_PGUP || key == KEY_PGDN) {
            console_view_scroll(key == KEY_PGUP ? ROWS - 1 : -(ROWS - 1));
            continue;
        }
        console_view_live();
        // up = an older line, down = a newer one, down from the newest gets back to an empty line
        // pressing past either end does nothing
        if (key == KEY_UP || key == KEY_DOWN) {
            if (key == KEY_UP && history_pos > 0) {
                history_pos--;
            } else if (key == KEY_DOWN && history_pos < input_history_count) {
                history_pos++;
            } else {
                continue;
            }
            // replace the current line with the history entry
            // first rub out what is on screen one character at a time...
            while (len > 0) {
                console_backspace();
                len--;
            }
            // ...then copy the entry in (cut to fit the caller's buffer) and print it
            const char* entry = (history_pos < input_history_count) ? input_history_get(history_pos) : "";
            strlcpy(in, entry, (size_t)size);
            len = (int)strlen(in);
            console_write(in);
            continue;
        }
        // Tab: let the caller finish the word (it prints whatever it adds, see IO.h)
        if (key == '\t' && complete) {
            len = complete(in, len, size);
            continue;
        }
        // a normal character: append it if there is still room for it and the NUL
        if (isprint(key) && len < size - 1) {
            in[len++] = (char)key;
            in[len] = '\0';
            console_putchar((char)key);
        }
    }

    input_history_add(in);
    return in;
}

char* GetString(char* in, int size) {
    return GetLine(in, size, NULL);
}

// state != 0 turns keyboard input on, 0 turns it off (kb_push then drops every key)
void InputBufferChangeState(int state) {
    kb_input_enabled = state != 0;
}

// empty the ring buffer right now by moving tail up to head, and ask the next GetString to do the
// same again, so keys typed while a program was busy (e.g. mashed during an animation) don't spill
// into the terminal prompt
void ClearInputbuffer(void) {
    kb_tail = kb_head;
    kb_clear_pending = true;
}

// send a byte to the 8042 controller once it is ready to take one
// port: KBC_COMMAND for a command, KBC_DATA for a command's data byte
// returns false if the controller never became ready (e.g. there is no PS/2 controller at all)
static bool kbc_write(uint16_t port, uint8_t value) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(KBC_STATUS) & KBC_INPUT_FULL) == 0) {
            outb(port, value);
            return true;
        }
    }
    return false;
}

// wait until the controller has a byte for us
// returns false on timeout, true when the byte can be read from KBC_DATA
static bool kbc_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(KBC_STATUS) & KBC_OUTPUT_FULL) {
            return true;
        }
    }
    return false;
}

// get the 8042 controller and keyboard ready and hook up IRQ1
// runs during boot while interrupts are still off, so no keyboard IRQ can arrive half way through.
// The keyboard itself isn't reset or reprogrammed: we rely on the BIOS having left it working and
// on the controller's translation to give us scancode set 1.
void init_keyboard(void) {
    // throw away anything left in the controller's output buffer
    // (key presses from while GRUB was running, for example). The loop is capped at 32 bytes in
    // case the status port reads as 0xFF forever on a machine without a controller.
    for (int i = 0; i < 32 && (inb(KBC_STATUS) & KBC_OUTPUT_FULL); i++) {
        inb(KBC_DATA);
    }

    // enable the keyboard IRQ and scancode translation (set 1) in the controller configuration byte
    // the configuration byte: bit 0 = first port (keyboard) IRQ, bit 1 = second port (mouse) IRQ,
    // bit 2 = system passed POST, bit 4 = first port clock DISABLED, bit 5 = second port clock
    // disabled, bit 6 = translate to set 1. We read it, change only our bits and write it back.
    if (kbc_write(KBC_COMMAND, 0x20) && kbc_wait_read()) { // read configuration byte
        uint8_t config = inb(KBC_DATA);
        config |= 0x01;           // first port interrupt (IRQ1) on
        config |= 0x40;           // translate to scancode set 1
        config &= (uint8_t)~0x10; // first port clock on
        // command 0x60 = "the next byte written to port 0x60 is the new configuration byte"
        if (kbc_write(KBC_COMMAND, 0x60)) { // write configuration byte
            kbc_write(KBC_DATA, config);
        }
    }

    // enable the first PS/2 port
    kbc_write(KBC_COMMAND, 0xAE);

    // route IRQ1 to keyboard_handler and unmask it at the PIC so the interrupt is actually delivered
    irq_install_handler(1, keyboard_handler);
    pic_unmask(1);
    kb_input_enabled = true;
    keyboard_ready = true;
}

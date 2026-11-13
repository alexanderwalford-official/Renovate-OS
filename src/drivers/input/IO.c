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
      The IRQ handler (producer) and getk (consumer) share a ring buffer. The handler only ever
      moves kb_head and getk only ever moves kb_tail, so on a single CPU they can't tread on
      each other's toes and no lock is needed. The indices are volatile so getk's wait loop
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

// (the 8042 controller's ports and status bits are in IO.h, as the mouse driver uses them too)

// where bytes from the controller's second port go: the mouse driver's packet reader while a
// program has the mouse on, NULL the rest of the time (see kb_set_mouse_sink)
static void (*volatile mouse_sink)(uint8_t byte) = NULL;

// number of slots in the key ring buffer, one is always left empty so it holds up to 127 keys
#define KB_BUFFER_SIZE 128

// the line editor remembers the last 8 lines, each up to 127 characters plus the NUL
#define INPUT_HISTORY_SIZE 8
#define INPUT_HISTORY_LEN  128

bool keyboard_ready = false;

static bool kb_input_enabled = false; // false = key presses are ignored
static bool kb_clear_pending = false; // set by ClearInputbuffer, handled by the next getln

// ring buffer filled by the interrupt handler and emptied by getk()
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
    // store the key first and only then publish it by moving head, so getk can never see a
    // slot that hasn't been filled in yet
    kb_buffer[kb_head] = key;
    kb_head = next;
}

// ---- keys held down (for games) -------------------------------------------------------------
//
// The buffer above only records key PRESSES, as characters, which is what typing wants. A game
// wants to know which keys are down right now ("is the left arrow held?"), so every press and
// release also updates this table. It is indexed by the key itself, whatever shift or caps lock
// are doing: the A key is always 'a', the arrows are KEY_LEFT and so on, shift is KEY_SHIFT
static volatile bool held[256];

// record that key went down (down = true) or came back up. Key 0 means "no key" and anything
// outside the table is ignored, so callers can pass whatever plain_key / extended_key returned
static void set_held(int key, bool down) {
    if (key > 0 && key < 256) {
        held[key] = down;
    }
}

// true while key is held down right now, see IO.h
bool kb_is_down(int key) {
    if (key >= 'A' && key <= 'Z') {
        key = key - 'A' + 'a'; // the table only has the lower case letters
    }
    if (key <= 0 || key >= 256) {
        return false;
    }
    return held[key];
}

// the key an ordinary (not 0xE0) scancode stands for, without shift: 0x1E is always 'a'.
// 0 for keys that don't have one
static int plain_key(uint8_t code) {
    // the modifiers have no character in the maps, so they get their KEY_ values here
    switch (code) {
        case 0x2A:
        case 0x36: return KEY_SHIFT;
        case 0x1D: return KEY_CTRL;
        case 0x38: return KEY_ALT;
    }
    // always the unshifted map, so shift + A still counts as the 'a' key being held.
    // The (uint8_t) stops a char above 127 turning into a negative number
    if (code < sizeof(kb_map_normal) - 1) {
        return (uint8_t)kb_map_normal[code];
    }
    // keypad 7 (0x47) is the first entry of kb_map_keypad
    if (code >= 0x47 && code <= 0x53) {
        return (uint8_t)kb_map_keypad[code - 0x47];
    }
    return 0;
}

// the key a scancode after the 0xE0 prefix stands for, 0 for keys that don't have one
static int extended_key(uint8_t code) {
    switch (code) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x53: return KEY_DELETE;
        case 0x49: return KEY_PGUP;
        case 0x51: return KEY_PGDN;
        case 0x1C: return '\n';       // keypad enter
        case 0x35: return '/';        // keypad slash
        case 0x1D: return KEY_CTRL;   // right ctrl
        case 0x38: return KEY_ALT;    // right alt
        default:   return 0;
    }
}

// keys that follow the 0xE0 prefix byte
// code: the scancode with the release bit already stripped, released: true for a key release
static void kb_process_extended(uint8_t code, bool released) {
    // some keyboards wrap keys like print screen or the arrows (with num lock on) in pretend
    // shift presses / releases: ignore them so they don't mess up kb_shift or the held keys
    if (code == 0x2A || code == 0x36) {
        return;
    }
    int key = extended_key(code); // 0 for keys AerOS doesn't use, like the Windows keys
    set_held(key, !released);

    // right ctrl matters on release too
    if (code == 0x1D) {
        kb_ctrl = !released;
        return;
    }
    // everything else only types something when pressed (and right alt types nothing)
    if (released || key == 0 || key == KEY_ALT) {
        return;
    }
    kb_push((uint8_t)key); // the cursor keys, Home, End..., keypad enter and slash
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

    // the byte before was 0xE0, so this is one of the newer keys with its own table
    if (kb_extended) {
        kb_extended = false;
        kb_process_extended(code, released);
        return;
    }

    // keep the held keys table up to date for every press and release, typing or not
    set_held(plain_key(code), !released);

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

// true once for every ctrl + C: reading the flag also clears it, so one press stops one program
// (there is a tiny window where an IRQ could set it between the read and the clear, losing that
// press, which is harmless)
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
    // the saved registers are not needed, this just silences the unused parameter warning
    (void)r;
    ps2_service();
}

// read the byte the controller is holding and hand it to whichever device sent it. The keyboard
// and the mouse share one data port, so the status register's "from the second port" bit is the
// only way to tell a mouse byte from a key press
void ps2_service(void) {
    uint8_t status = inb(KBC_STATUS);
    // nothing waiting (another handler already took it): reading now would give the last byte
    // a second time, which would type a key twice
    if (!(status & KBC_OUTPUT_FULL)) {
        return;
    }
    uint8_t byte = inb(KBC_DATA);
    crypto_add_entropy(byte); // key press and mouse timing feed the random number generator
    if (status & KBC_FROM_MOUSE) {
        // a mouse byte is never a key, even when no one is listening for the mouse
        void (*sink)(uint8_t) = mouse_sink;
        if (sink != NULL) {
            sink(byte);
        }
        return;
    }
    kb_process_scancode(byte);
}

// see IO.h. One pointer write, so an IRQ in the middle sees the old sink or the new one
void kb_set_mouse_sink(void (*sink)(uint8_t byte)) {
    mouse_sink = sink;
}

// like getln, but shows '*' for every character and keeps no history (for passwords)
char* GetSecret (char* in, int size) {
    int len = 0;
    for (;;) {
        int key = getk();
        // no keyboard, or enter: the secret is finished
        if (key == 0 || key == '\n' || key == '\r') {
            break;
        }
        // backspace rubs out one star as well as the character behind it
        if (key == '\b') {
            if (len > 0) {
                len--;
                console_backspace();
            }
        } else if (isprint(key) && len < size - 1) {
            // keep the real character but only ever show a star
            in[len++] = (char)key;
            console_putchar('*');
        }
    }
    // the loop only stores characters, the NUL goes on at the end
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
int getk(void) {
    if (!keyboard_ready || !interrupts_enabled()) {
        return 0; // no keyboard interrupts, waiting would hang forever
    }
    if (console_capturing()) {
        return 0; // a command run by AerLang's cmd(): nobody can see what it is asking
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
    // keep reading until a plain character arrives
    do {
        key = getk();
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
    if (input_history_count > 0 && scmp(input_history_get(input_history_count - 1), line) == 0) {
        return; // same as the last line
    }
    scpy(input_history[input_history_next], line, INPUT_HISTORY_LEN);
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

// ---- the line being edited, on screen -------------------------------------------------------
//
// The line editor lets the cursor move inside the line (Left, Right, Home, End), so it has to know
// where on the screen each character of the line is. A line can wrap onto the rows below, so it
// is kept as one number: the screen cell its first character is in, counting cells from the top
// left (row * COLS + column). Character i of the line is then in cell line_start + i.
// When typing at the bottom of the screen scrolls it up, everything moves up a row, line_start
// with it: after anything is written, the cursor is checked against where it should have ended up,
// and the difference is how far the screen scrolled. (line_start can go below 0 when the start of a
// long line has scrolled off the top, which is fine: those cells are just never drawn again.)
static int line_start;

// the cell the console cursor is in, counting from the top left
static int cursor_cell(void) {
    uint8_t x, y;
    console_get_cursor(&x, &y);
    return y * COLS + x;
}

// put the cursor on character `offset` of the line (offset == length: just after the end)
static void line_cursor_to(int offset) {
    int cell = line_start + offset;
    if (cell < 0) {
        cell = 0; // that part of the line has scrolled off the top: the best we can do
    }
    console_set_cursor((uint8_t)(cell % COLS), (uint8_t)(cell / COLS));
}

// after writing up to character `offset` of the line, the cursor should be just after it. If it
// is higher up, the screen scrolled while writing, by exactly that much: move the line up with it
static void line_follow_scroll(int offset) {
    int scrolled = line_start + offset - cursor_cell();
    if (scrolled > 0) {
        line_start -= scrolled;
    }
}

// draw the line from character `from` to its end, then `erase` spaces to rub out what was there
// before (a character deleted from the middle leaves the old last one behind). The cursor is
// left wherever the caller puts it next
static void line_redraw(const char* in, int from, int length, int erase) {
    line_cursor_to(from);
    for (int i = from; i < length; i++) {
        console_putchar_raw(in[i]);
    }
    for (int i = 0; i < erase; i++) {
        console_putchar_raw(' ');
    }
    line_follow_scroll(length + erase);
}

// the line editor: read keys until enter, echoing them and keeping in up to date as we go
// in: buffer that receives the line, size: its size in bytes (the line is at most size - 1 characters)
// returns in, which is left empty if there is no keyboard
// supported keys: printable characters (typed in where the cursor is), Left / Right, Home / End,
// Backspace (the character before the cursor) and Delete (the one under it), Enter, Up / Down for
// the history, Page Up / Page Down to look back at earlier output, and Tab if a complete function
// was given. Anything else (escape, ctrl codes...) is ignored.
char* GetLine(char* in, int size, tab_complete_fn complete) {
    if (kb_clear_pending) {
        // throw away keys that were pressed between the clear and this point
        kb_tail = kb_head;
        kb_clear_pending = false;
    }

    int len = 0;   // how long the line is
    int pos = 0;   // where the cursor is in it: 0 = before the first character, len = after the last
    int history_pos = input_history_count; // one past the newest entry = the line being typed
    in[0] = '\0';
    line_start = cursor_cell(); // the line starts where the cursor is now, after the prompt

    for (;;) {
        int key = getk();
        if (key == 0) {
            break; // no keyboard
        }
        // enter (or ctrl + M) finishes the line, wherever the cursor is in it
        if (key == '\n' || key == '\r') {
            line_cursor_to(len);
            console_putchar('\n');
            break;
        }
        // Page Up / Page Down look back through what has scrolled off the top of the screen, a
        // screen at a time (one line less, so the line at the edge stays in view). Any other key
        // comes back to the live screen first, so typing carries on where it was
        if (key == KEY_PGUP || key == KEY_PGDN) {
            console_view_scroll(key == KEY_PGUP ? ROWS - 1 : -(ROWS - 1));
            continue;
        }
        console_view_live();

        // moving along the line: nothing in it changes, only where the cursor is
        if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_HOME || key == KEY_END) {
            if (key == KEY_LEFT && pos > 0) {
                pos--;
            } else if (key == KEY_RIGHT && pos < len) {
                pos++;
            } else if (key == KEY_HOME) {
                pos = 0;
            } else if (key == KEY_END) {
                pos = len;
            }
            line_cursor_to(pos);
            continue;
        }

        // backspace: drop the character before the cursor
        if (key == '\b') {
            if (pos == 0) {
                continue;
            }
            if (pos == len) {
                // at the end, the quick way: rub out the last character on screen
                in[--len] = '\0';
                pos--;
                console_backspace();
                continue;
            }
            // in the middle: close the gap (memmove, as the two areas overlap, and len - pos + 1
            // takes the '\0' along), then redraw from the gap on, with a space over the old end
            memmove(in + pos - 1, in + pos, (size_t)(len - pos + 1));
            len--;
            pos--;
            line_redraw(in, pos, len, 1);
            line_cursor_to(pos);
            continue;
        }
        // delete: drop the character under the cursor (nothing at the end of the line)
        if (key == KEY_DELETE) {
            if (pos < len) {
                memmove(in + pos, in + pos + 1, (size_t)(len - pos));
                len--;
                line_redraw(in, pos, len, 1);
                line_cursor_to(pos);
            }
            continue;
        }

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
            // replace the current line with the history entry: draw the entry over it, with
            // spaces over whatever of the old line is left past its end...
            int old_len = len;
            const char* entry = (history_pos < input_history_count) ? input_history_get(history_pos) : "";
            scpy(in, entry, (size_t)size);
            len = (int)slen(in);
            line_redraw(in, 0, len, old_len > len ? old_len - len : 0);
            // ...and the cursor goes to its end, ready to carry on typing
            pos = len;
            line_cursor_to(pos);
            continue;
        }
        // Tab: let the caller finish the word (it prints whatever it adds, see IO.h). It works
        // on the end of the line, so the cursor goes there first. It may print a list of choices
        // and the line again below them, so afterwards the line starts len characters before
        // wherever the cursor has ended up
        if (key == '\t' && complete) {
            line_cursor_to(len);
            len = complete(in, len, size);
            pos = len;
            line_start = cursor_cell() - len;
            continue;
        }
        // a normal character: type it in at the cursor, if there is still room for it and the NUL
        if (isprint(key) && len < size - 1) {
            if (pos == len) {
                // at the end, the quick way: just print it
                in[len++] = (char)key;
                in[len] = '\0';
                pos++;
                console_putchar((char)key);
                line_follow_scroll(pos);
                continue;
            }
            // in the middle: open a gap (moving the '\0' too), put it in, and redraw from there
            memmove(in + pos + 1, in + pos, (size_t)(len - pos + 1));
            in[pos] = (char)key;
            len++;
            line_redraw(in, pos, len, 0);
            pos++;
            line_cursor_to(pos);
        }
    }

    // finished (or no keyboard): remember the line for the up arrow
    input_history_add(in);
    return in;
}

// the line editor without Tab completion, see IO.h
char* getln(char* in, int size) {
    return GetLine(in, size, NULL);
}

// state != 0 turns keyboard input on, 0 turns it off (kb_push then drops every key)
void InputBufferChangeState(int state) {
    kb_input_enabled = state != 0;
}

// empty the ring buffer right now by moving tail up to head, and ask the next getln to do the
// same again, so keys typed while a program was busy (e.g. mashed during an animation) don't spill
// into the terminal prompt
void ClearInputbuffer(void) {
    kb_tail = kb_head;
    kb_clear_pending = true;
}

// send a byte to the 8042 controller once it is ready to take one
// port: KBC_COMMAND for a command, KBC_DATA for a command's data byte
// returns false if the controller never became ready (e.g. there is no PS/2 controller at all)
bool kbc_write(uint16_t port, uint8_t value) {
    // poll the status register (capped, so a missing controller can't hang the boot) until the
    // controller has taken its previous byte
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
bool kbc_wait_read(void) {
    // the same capped poll as kbc_write, this time for the output full bit
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

    // enable the first PS/2 port (command 0xAE, in case the BIOS left it disabled)
    kbc_write(KBC_COMMAND, 0xAE);

    // route IRQ1 to keyboard_handler and unmask it at the PIC so the interrupt is actually delivered
    irq_install_handler(1, keyboard_handler);
    pic_unmask(1);
    kb_input_enabled = true;
    keyboard_ready = true;
}

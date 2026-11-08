/*
    IO.c
    Alexander Walford 2023

    PS/2 keyboard driver: IRQ1 handler, scancode translation, input buffer and line editor.
*/

#include "IO.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "GIO.h"
#include "../video/VGA_linear.h"
#include "../../lib/ren_ctype.h"
#include "../../lib/ren_string.h"
#include "../../lib/ren_stdio.h"
#include "../../sys/interrupts.h"

#define KBC_DATA    0x60
#define KBC_STATUS  0x64
#define KBC_COMMAND 0x64

// controller status bits
#define KBC_OUTPUT_FULL 0x01 // a byte is waiting for us
#define KBC_INPUT_FULL  0x02 // the controller has not taken the last byte yet

#define KB_BUFFER_SIZE 128

#define INPUT_HISTORY_SIZE 8
#define INPUT_HISTORY_LEN  128

bool keyboard_ready = false;

static bool kb_input_enabled = false; // false = key presses are ignored
static bool kb_clear_pending = false; // set by ClearInputbuffer, handled by the next GetString

// ring buffer filled by the interrupt handler and emptied by GetChar()
static volatile uint8_t kb_buffer[KB_BUFFER_SIZE];
static volatile unsigned int kb_head = 0;
static volatile unsigned int kb_tail = 0;

// modifier key state
static bool kb_shift = false;
static bool kb_ctrl = false;
static bool kb_caps = false;
static bool kb_extended = false;

// US QWERTY, scancode set 1 (index = scancode)
static const char kb_map_normal[] =
    "\0\x1B" "1234567890-=" "\b"
    "\t" "qwertyuiop[]" "\n"
    "\0" "asdfghjkl;'`"
    "\0" "\\zxcvbnm,./" "\0"
    "*" "\0" " ";

static const char kb_map_shift[] =
    "\0\x1B" "!@#$%^&*()_+" "\b"
    "\t" "QWERTYUIOP{}" "\n"
    "\0" "ASDFGHJKL:\"~"
    "\0" "|ZXCVBNM<>?" "\0"
    "*" "\0" " ";

// keypad keys 0x47 - 0x53
static const char kb_map_keypad[] = "789-456+1230.";

static char input_history[INPUT_HISTORY_SIZE][INPUT_HISTORY_LEN];
static int input_history_count = 0; // number of stored lines
static int input_history_next = 0;  // slot the next line goes into

static void kb_push(uint8_t key) {
    if (!kb_input_enabled) {
        return;
    }
    unsigned int next = (kb_head + 1) % KB_BUFFER_SIZE;
    if (next == kb_tail) {
        return; // buffer full, drop the key
    }
    kb_buffer[kb_head] = key;
    kb_head = next;
}

// keys that follow the 0xE0 prefix byte
static void kb_process_extended(uint8_t code, bool released) {
    switch (code) {
        case 0x1D: kb_ctrl = !released; return; // right ctrl
        case 0x2A:
        case 0x36: return;                      // fake shifts sent with some extended keys
    }
    if (released) {
        return;
    }
    switch (code) {
        case 0x48: kb_push(KEY_UP); break;
        case 0x50: kb_push(KEY_DOWN); break;
        case 0x4B: kb_push(KEY_LEFT); break;
        case 0x4D: kb_push(KEY_RIGHT); break;
        case 0x47: kb_push(KEY_HOME); break;
        case 0x4F: kb_push(KEY_END); break;
        case 0x53: kb_push(KEY_DELETE); break;
        case 0x1C: kb_push('\n'); break; // keypad enter
        case 0x35: kb_push('/'); break;  // keypad slash
    }
}

// translate one scancode into a key and store it
static void kb_process_scancode(uint8_t scancode) {
    if (scancode == 0xE0) {
        kb_extended = true;
        return;
    }

    bool released = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    if (kb_extended) {
        kb_extended = false;
        kb_process_extended(code, released);
        return;
    }

    switch (code) {
        case 0x2A:
        case 0x36: kb_shift = !released; return; // left / right shift
        case 0x1D: kb_ctrl = !released; return;  // left ctrl
        case 0x3A: if (!released) kb_caps = !kb_caps; return;
    }
    if (released) {
        return;
    }

    char c = 0;
    if (code < sizeof(kb_map_normal) - 1) {
        c = kb_shift ? kb_map_shift[code] : kb_map_normal[code];
    } else if (code >= 0x47 && code <= 0x53) {
        c = kb_map_keypad[code - 0x47];
    }
    if (c == 0) {
        return; // function keys and other keys without a character
    }
    if (kb_caps && isalpha(c)) {
        c = (char)(isupper(c) ? tolower(c) : toupper(c));
    }
    if (kb_ctrl && isalpha(c)) {
        c = (char)(toupper(c) - 'A' + 1); // ctrl + letter = control code 1-26
    }
    kb_push((uint8_t)c);
}

// IRQ1 handler
static void keyboard_handler(struct regs* r) {
    (void)r;
    kb_process_scancode(inb(KBC_DATA));
}

bool kb_haschar(void) {
    return kb_head != kb_tail;
}

int GetChar(void) {
    if (!keyboard_ready || !interrupts_enabled()) {
        return 0; // no keyboard interrupts, waiting would hang forever
    }
    while (!kb_haschar()) {
        cpu_idle(); // sleep until the next interrupt
    }
    uint8_t key = kb_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;
    return key;
}

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
static const char* input_history_get(int index) {
    int slot = (input_history_next - input_history_count + index + INPUT_HISTORY_SIZE) % INPUT_HISTORY_SIZE;
    return input_history[slot];
}

static void input_history_add(const char* line) {
    if (line[0] == '\0') {
        return;
    }
    if (input_history_count > 0 && strcmp(input_history_get(input_history_count - 1), line) == 0) {
        return; // same as the last line
    }
    strlcpy(input_history[input_history_next], line, INPUT_HISTORY_LEN);
    input_history_next = (input_history_next + 1) % INPUT_HISTORY_SIZE;
    if (input_history_count < INPUT_HISTORY_SIZE) {
        input_history_count++;
    }
}

char* GetString(char* in, int size) {
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
        if (key == '\n' || key == '\r') {
            console_putchar('\n');
            break;
        }
        if (key == '\b') {
            if (len > 0) {
                in[--len] = '\0';
                console_backspace();
            }
            continue;
        }
        if (key == KEY_UP || key == KEY_DOWN) {
            if (key == KEY_UP && history_pos > 0) {
                history_pos--;
            } else if (key == KEY_DOWN && history_pos < input_history_count) {
                history_pos++;
            } else {
                continue;
            }
            // replace the current line with the history entry
            while (len > 0) {
                console_backspace();
                len--;
            }
            const char* entry = (history_pos < input_history_count) ? input_history_get(history_pos) : "";
            strlcpy(in, entry, (size_t)size);
            len = (int)strlen(in);
            console_write(in);
            continue;
        }
        if (isprint(key) && len < size - 1) {
            in[len++] = (char)key;
            in[len] = '\0';
            console_putchar((char)key);
        }
    }

    input_history_add(in);
    return in;
}

void InputBufferChangeState(int state) {
    kb_input_enabled = state != 0;
}

void ClearInputbuffer(void) {
    kb_tail = kb_head;
    kb_clear_pending = true;
}

// send a byte to the 8042 controller once it is ready to take one
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
static bool kbc_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(KBC_STATUS) & KBC_OUTPUT_FULL) {
            return true;
        }
    }
    return false;
}

void init_keyboard(void) {
    // throw away anything left in the controller's output buffer
    for (int i = 0; i < 32 && (inb(KBC_STATUS) & KBC_OUTPUT_FULL); i++) {
        inb(KBC_DATA);
    }

    // enable the keyboard IRQ and scancode translation (set 1) in the controller configuration byte
    if (kbc_write(KBC_COMMAND, 0x20) && kbc_wait_read()) { // read configuration byte
        uint8_t config = inb(KBC_DATA);
        config |= 0x01;           // first port interrupt (IRQ1) on
        config |= 0x40;           // translate to scancode set 1
        config &= (uint8_t)~0x10; // first port clock on
        if (kbc_write(KBC_COMMAND, 0x60)) { // write configuration byte
            kbc_write(KBC_DATA, config);
        }
    }

    // enable the first PS/2 port
    kbc_write(KBC_COMMAND, 0xAE);

    irq_install_handler(1, keyboard_handler);
    pic_unmask(1);
    kb_input_enabled = true;
    keyboard_ready = true;
}

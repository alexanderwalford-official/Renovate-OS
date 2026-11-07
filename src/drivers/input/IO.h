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

#include <stdint.h>
#include <stdbool.h>
#include "GIO.h"
#include "../video/VGA_linear.h"
#include "../../lib/primary_definitions.h"
#include "../../sys/interrupts.h"

#define KBC_DATA    0x60
#define KBC_STATUS  0x64
#define KBC_COMMAND 0x64

// special (non-ASCII) keys returned by GetChar()
#define KEY_ESCAPE 27
#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_HOME   0x84
#define KEY_END    0x85
#define KEY_DELETE 0x86

#define KB_BUFFER_SIZE 128

int InputBufferBusy = 0; // input buffer state, 1 = key presses are stored, 0 = they are ignored
int ClearBuffer = 0;
bool keyboard_ready = false;

// ring buffer filled by the interrupt handler and emptied by GetChar()
volatile uint8_t kb_buffer[KB_BUFFER_SIZE];
volatile unsigned int kb_head = 0;
volatile unsigned int kb_tail = 0;

// modifier key state
bool kb_shift = false;
bool kb_ctrl = false;
bool kb_alt = false;
bool kb_caps = false;
bool kb_extended = false;

// US QWERTY, scancode set 1 (index = scancode)
const char kb_map_normal[] =
    "\0\x1B" "1234567890-=" "\b"
    "\t" "qwertyuiop[]" "\n"
    "\0" "asdfghjkl;'`"
    "\0" "\\zxcvbnm,./" "\0"
    "*" "\0" " ";

const char kb_map_shift[] =
    "\0\x1B" "!@#$%^&*()_+" "\b"
    "\t" "QWERTYUIOP{}" "\n"
    "\0" "ASDFGHJKL:\"~"
    "\0" "|ZXCVBNM<>?" "\0"
    "*" "\0" " ";

// keypad keys 0x47 - 0x53
const char kb_map_keypad[] = "789-456+1230.";

void kb_push(uint8_t key) {
    if (!InputBufferBusy) {
        return;
    }
    unsigned int next = (kb_head + 1) % KB_BUFFER_SIZE;
    if (next == kb_tail) {
        return; // buffer full, drop the key
    }
    kb_buffer[kb_head] = key;
    kb_head = next;
}

// translate one scancode into a key and store it
void kb_process_scancode(uint8_t scancode) {
    if (scancode == 0xE0) {
        kb_extended = true;
        return;
    }

    bool released = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    if (kb_extended) {
        kb_extended = false;
        switch (code) {
            case 0x1D: kb_ctrl = !released; return; // right ctrl
            case 0x38: kb_alt = !released; return;  // right alt
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
        return;
    }

    switch (code) {
        case 0x2A:
        case 0x36: kb_shift = !released; return; // left / right shift
        case 0x1D: kb_ctrl = !released; return;  // left ctrl
        case 0x38: kb_alt = !released; return;   // left alt
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
    if (kb_caps && isalpha((unsigned char)c)) {
        c = (char)(isupper((unsigned char)c) ? tolower((unsigned char)c) : toupper((unsigned char)c));
    }
    if (kb_ctrl && isalpha((unsigned char)c)) {
        c = (char)(toupper((unsigned char)c) - 'A' + 1); // ctrl + letter = control code 1-26
    }
    kb_push((uint8_t)c);
}

// IRQ1 handler
void keyboard_handler(struct regs* r) {
    (void)r;
    uint8_t scancode = inb(KBC_DATA);
    kb_process_scancode(scancode);
}

// returns true if a key is waiting in the input buffer
bool kb_haschar(void) {
    return kb_head != kb_tail;
}

// returns the next pressed key, waiting for one if the buffer is empty
// (ASCII characters, or one of the KEY_ special values)
int GetChar(void)
{
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

// standard style getchar(), waits for the next printable / control character
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

/* ---------------- line input ---------------- */

#define INPUT_HISTORY_SIZE 8
#define INPUT_HISTORY_LEN  128

char input_history[INPUT_HISTORY_SIZE][INPUT_HISTORY_LEN];
int input_history_count = 0; // number of stored lines
int input_history_next = 0;  // slot the next line goes into

// index 0 = oldest stored line
const char* input_history_get(int index) {
    int slot = (input_history_next - input_history_count + index + INPUT_HISTORY_SIZE) % INPUT_HISTORY_SIZE;
    return input_history[slot];
}

void input_history_add(const char* line) {
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

// get input string: reads a line from the keyboard into in (at most size - 1 characters),
// echoing it to the console. Supports backspace and the up / down arrows for previous lines.
char* GetString (char* in, int size) {
    if (in == NULL || size <= 0) {
        return in;
    }
    if (ClearBuffer == 1) {
        // a clear was requested, throw away keys that were pressed before this point
        kb_tail = kb_head;
        ClearBuffer = 0;
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

    in[len] = '\0';
    input_history_add(in);
    return in;
}

// parse either 1 or 0 to enable to disable input buffer for current string
void InputBufferChangeState (int state) {
  InputBufferBusy = state ? 1 : 0;
  return;
}

// throw away every key press that has not been read yet
void ClearInputbuffer () {
  kb_tail = kb_head;
  ClearBuffer = 1;
  return;
}

/* ---------------- 8042 controller ---------------- */

// wait until the controller is ready to accept a byte
bool kbc_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(KBC_STATUS) & 0x02) == 0) return true;
    }
    return false;
}

// wait until the controller has a byte for us
bool kbc_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(KBC_STATUS) & 0x01) return true;
    }
    return false;
}

// function to initialize the keyboard (requires idt_init() first)
void init_keyboard() {
    // throw away anything left in the controller's output buffer
    for (int i = 0; i < 32 && (inb(KBC_STATUS) & 0x01); i++) {
        inb(KBC_DATA);
    }

    // enable the keyboard IRQ and scancode translation (set 1) in the controller configuration byte
    if (kbc_wait_write()) {
        outb(KBC_COMMAND, 0x20); // read configuration byte
        if (kbc_wait_read()) {
            uint8_t config = inb(KBC_DATA);
            config |= 0x01;  // first port interrupt (IRQ1) on
            config |= 0x40;  // translate to scancode set 1
            config &= (uint8_t)~0x10; // first port clock on
            if (kbc_wait_write()) {
                outb(KBC_COMMAND, 0x60); // write configuration byte
                if (kbc_wait_write()) {
                    outb(KBC_DATA, config);
                }
            }
        }
    }

    // enable the first PS/2 port
    if (kbc_wait_write()) {
        outb(KBC_COMMAND, 0xAE);
    }

    irq_install_handler(1, keyboard_handler);
    pic_unmask(1);
    InputBufferBusy = 1;
    keyboard_ready = true;
}

#endif /* IO_H */

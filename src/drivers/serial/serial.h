/*
    serial.h
    Minimal COM1 serial port driver, used to mirror console output for debugging.
    Run QEMU with "-serial stdio" to see everything the terminal prints.
    https://wiki.osdev.org/Serial_Ports
*/
#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stdbool.h>
#include "../input/GIO.h"

#define COM1_PORT 0x3F8

bool serial_ready = false;

// initialise COM1 at 38400 baud, 8N1. Does nothing if no serial port is present.
void serial_init(void) {
    outb(COM1_PORT + 1, 0x00); // disable serial interrupts
    outb(COM1_PORT + 3, 0x80); // enable DLAB (baud rate divisor)
    outb(COM1_PORT + 0, 0x03); // divisor low byte (38400 baud)
    outb(COM1_PORT + 1, 0x00); // divisor high byte
    outb(COM1_PORT + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(COM1_PORT + 2, 0xC7); // enable FIFO, clear it, 14 byte threshold
    outb(COM1_PORT + 4, 0x1E); // loopback mode to test the chip
    outb(COM1_PORT + 0, 0xAE); // send a test byte

    if (inb(COM1_PORT + 0) != 0xAE) {
        serial_ready = false; // no (working) serial port
        return;
    }

    outb(COM1_PORT + 4, 0x0F); // normal operation mode
    serial_ready = true;
}

// write a single character to COM1
void serial_putchar(char c) {
    if (!serial_ready) {
        return;
    }
    // wait (with a timeout) until the transmit buffer is empty
    for (int timeout = 100000; timeout > 0 && (inb(COM1_PORT + 5) & 0x20) == 0; --timeout) {
    }
    outb(COM1_PORT, (uint8_t)c);
}

// write a string to COM1
void serial_write(const char* s) {
    if (s == 0) {
        return;
    }
    for (; *s; ++s) {
        if (*s == '\n') {
            serial_putchar('\r');
        }
        serial_putchar(*s);
    }
}

#endif /* SERIAL_H */

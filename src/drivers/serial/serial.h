/*
    serial.h
    Minimal COM1 serial port driver, used to mirror console output for debugging.
    Run QEMU with "-serial stdio" to see everything the terminal prints.
    https://wiki.osdev.org/Serial_Ports
*/
#ifndef SERIAL_H
#define SERIAL_H

// initialise COM1 at 38400 baud, 8N1. Does nothing if no serial port is present.
void serial_init(void);

void serial_putchar(char c);

// write a string to COM1, '\n' is sent as "\r\n"
void serial_write(const char* s);

#endif /* SERIAL_H */

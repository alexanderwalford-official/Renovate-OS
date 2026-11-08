/*
    serial.h
    Minimal COM1 serial port driver, used to mirror console output for debugging.
    Run QEMU with "-serial stdio" to see everything the terminal prints.
    https://wiki.osdev.org/Serial_Ports

    Why bother with a serial port? The VGA screen only holds 25 lines and disappears the moment
    the machine resets, while the serial port streams every character out of the machine. QEMU
    can print it in your terminal or save it to a file ("-serial file:log.txt"), so it is the
    easiest way to read what happened just before a crash.

    The console in drivers/video/VGA_linear.c calls serial_putchar for every character it draws,
    so nothing else in the OS has to know the serial port exists. The driver only sends: there is
    no receive side and no serial interrupts.
*/
#ifndef SERIAL_H
#define SERIAL_H

// initialise COM1 at 38400 baud, 8N1. Does nothing if no serial port is present.
// (8N1 = 8 data bits, No parity bit, 1 stop bit, the setting almost everything expects)
// called once by the kernel during boot, before that serial_putchar quietly drops everything
void serial_init(void);

// send one raw byte to COM1 (no '\n' translation), silently ignored if there is no serial port
void serial_putchar(char c);

// write a string to COM1, '\n' is sent as "\r\n"
void serial_write(const char* s);

#endif /* SERIAL_H */

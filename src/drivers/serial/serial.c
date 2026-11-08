/*
    serial.c
    Minimal COM1 serial port driver.
    https://wiki.osdev.org/Serial_Ports

    A PC serial port is driven by a UART chip (the 16550 and its compatible descendants; QEMU
    emulates one). The UART turns bytes into a stream of bits on a wire. It has 8 registers,
    reached through 8 consecutive I/O ports starting at the port's base address (COM1 = 0x3F8):

      offset  read                          write
      +0      receive buffer                transmit holding register (THR)
      +1      interrupt enable              interrupt enable
      +2      interrupt identification      FIFO control
      +3      line control (LCR)            line control (LCR)
      +4      modem control (MCR)           modem control (MCR)
      +5      line status (LSR)             -
      +6      modem status                  -
      +7      scratch                       scratch

    Bit 7 of the line control register is DLAB (Divisor Latch Access Bit). While it is set,
    offsets +0 and +1 stop being the data / interrupt registers and become the low / high byte
    of the baud rate divisor instead. The UART runs from a 115200 Hz reference, so the baud rate
    is 115200 / divisor: divisor 1 = 115200 baud, 3 = 38400 baud, 12 = 9600 baud.

    The only status bit we care about is LSR bit 5 (0x20, "transmitter holding register empty"),
    which says the UART is ready for the next byte.
*/
#include "serial.h"

#include <stdint.h>
#include <stdbool.h>
#include "../input/GIO.h"

// base I/O port of the first serial port (COM2 would be 0x2F8, COM3 0x3E8, COM4 0x2E8)
#define COM1_PORT 0x3F8

// true once serial_init found a working UART, until then every write is skipped
// (on a machine without a serial port, waiting for "transmit empty" would just time out every time)
static bool serial_ready = false;

// set COM1 up for 38400 baud 8N1 with the FIFOs on, then check the chip really exists by
// sending a byte to ourselves in loopback mode. Only if it comes back is serial_ready set.
void serial_init(void) {
    outb(COM1_PORT + 1, 0x00); // disable serial interrupts
    // the next two writes go to the divisor latch because DLAB is set
    outb(COM1_PORT + 3, 0x80); // enable DLAB (baud rate divisor)
    outb(COM1_PORT + 0, 0x03); // divisor low byte (38400 baud)
    outb(COM1_PORT + 1, 0x00); // divisor high byte
    // 0x03 = word length bits 0-1 set (8 bits), bit 2 clear (1 stop bit), bits 3-5 clear (no
    // parity) and bit 7 clear, which also switches DLAB off again
    outb(COM1_PORT + 3, 0x03); // 8 bits, no parity, one stop bit
    // 0xC7 = bit 0 FIFO on, bit 1 clear receive FIFO, bit 2 clear transmit FIFO,
    // bits 6-7 = interrupt trigger level of 14 bytes (we don't use interrupts, but it is the usual value)
    outb(COM1_PORT + 2, 0xC7); // enable FIFO, clear it, 14 byte threshold
    // 0x1E = RTS (bit 1), OUT1 (bit 2), OUT2 (bit 3) and LOOP (bit 4). In loopback mode the
    // transmitter is wired straight back into the receiver inside the chip, nothing goes out
    outb(COM1_PORT + 4, 0x1E); // loopback mode to test the chip
    outb(COM1_PORT + 0, 0xAE); // send a test byte

    // only a working serial port sends the test byte back
    // (with no UART at this address the read usually floats to 0xFF, a broken one returns junk)
    if (inb(COM1_PORT + 0) == 0xAE) {
        // 0x0F = DTR, RTS, OUT1 and OUT2 on, LOOP off, so bytes now really leave the machine
        outb(COM1_PORT + 4, 0x0F); // normal operation mode
        serial_ready = true;
    }
}

// send one byte over COM1
// c: the byte to send, no translation is done (serial_write handles "\n" -> "\r\n")
// gotcha: if the UART never reports ready the byte is written anyway after the timeout, so a stuck
// port loses characters instead of hanging the whole kernel
void serial_putchar(char c) {
    if (!serial_ready) {
        return;
    }
    // wait (with a timeout) until the transmit buffer is empty
    // (polls LSR bit 5; the loop body is empty on purpose, all the work happens in the condition)
    for (int timeout = 100000; timeout > 0 && (inb(COM1_PORT + 5) & 0x20) == 0; timeout--) {
    }
    outb(COM1_PORT, (uint8_t)c);
}

// send a NUL terminated string over COM1
// terminals treat '\n' as "move down a line" and '\r' as "back to column 0", so to start a fresh
// line on the other end each '\n' is preceded by a '\r'
void serial_write(const char* s) {
    for (; *s; s++) {
        if (*s == '\n') {
            serial_putchar('\r');
        }
        serial_putchar(*s);
    }
}

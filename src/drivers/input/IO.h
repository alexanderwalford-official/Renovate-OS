/*
    IO.h
    Alexander Walford 2023
*/

#ifndef IO_H
#define IO_H

#include "stdint.h"
#include "../video/VGA_linear.h"

// use uint16_t (16 bit unsigned integer) for 16bit keyboard signals

int InputBufferBusy = 0; // input buffer state
int ClearBuffer = 0;

// returns the pressed character
int GetChar(void)
{
  uint16_t ax;
	__asm__ ("int $0x16" : "+a" (ax));
  return ax & 0xff;
}

// get input string
char GetString (char in[]) {
  if (ClearBuffer == 1) {
    in = NewString();
  }
  if (InputBufferBusy == 1) {
    int size_of_buffer = sizeof(in);
    int key = GetChar();
    if (key != '\r' && key != '\b' && key != '\0' && key !=' ') {
      //in[size_of_buffer] = key;
    }
  }
  return in;
}

// parse either 1 or 0 to enable to disable input buffer for current string
void InputBufferChangeState (int state) {
  InputBufferBusy = state;
  if (state == 0) {
    puts(0, 20, RED, BLACK, "[BFS0]");
  }
  else {
    puts(0, 20, RED, BLACK, "[BFS1]");
  }
  return;
}

void ClearInputbuffer () {
  ClearBuffer = 1;
  return;
}

// convert int to string
char* itoa(int value, char* str, int base)
{
    char* rc;
    char* ptr;
    char* low;
    // Check for supported base.
    if ( base < 2 || base > 36 )
    {
        *str = '\0';
        return str;
    }
    rc = ptr = str;
    // Set '-' for negative decimals.
    if ( value < 0 && base == 10 )
    {
        *ptr++ = '-';
    }
    // Remember where the numbers start.
    low = ptr;
    // The actual conversion.
    do
    {
        // Modulo is negative for negative value. This trick makes abs() unnecessary.
        *ptr++ = "zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz"[35 + value % base];
        value /= base;
    } while ( value );
    // Terminating the string.
    *ptr-- = '\0';
    // Invert the numbers.
    while ( low < ptr )
    {
        char tmp = *low;
        *low++ = *ptr;
        *ptr-- = tmp;
    }
    return rc;
}

// the code below this point requires testing

// function to write a byte to a specified port
void outb(uint16_t port, uint8_t value) {
    asm volatile ("outb %0, %1" : : "a" (value), "Nd" (port));
}

// function to read a byte from a specified port
uint8_t inb(uint16_t port) {
    uint8_t result;
    asm volatile ("inb %1, %0" : "=a" (result) : "Nd" (port));
    return result;
}

// function to initialize the keyboard
void init_keyboard() {
    // Disable interrupts
    asm volatile ("cli");

    // enable the keyboard by sending the appropriate command to the controller
    // for example, to enable the keyboard (bit 0 of port 0x61)
    uint8_t current = inb(0x61);
    outb(0x61, current | 0x01);

    // enable interrupts
    asm volatile ("sti");
}


#endif /* IO_H */

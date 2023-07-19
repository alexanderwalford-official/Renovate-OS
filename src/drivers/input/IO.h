/*
    IO.h
    Alexander Walford 2023
*/

#ifndef IO_H
#define IO_H

#include "stdint.h"

// use uint16_t (16 bit unsigned integer) for 16bit keyboard signals

// experimental methods:

void PrintChar (char out, uint16_t attr)
{
   __asm__ ("int $0x10\n\t"
            :
            : "a"((0x0e << 8) | out),
              "b"(attr));
}

char GetChar(void) {
  uint16_t in;
  __asm__ __volatile__ ("int $0x16\n\t"
    : "=a"(in)
    : "0"(0x0));
  return ((char)in);
}

// formulate a string from the GetChar method
char *GetString(char *InputBuffer, int MaxChars) {
  char *BufferPtr = InputBuffer;
  while(BufferPtr < (InputBuffer + MaxChars) && (*BufferPtr == GetChar()) != '\r') {
    PrintChar(*BufferPtr++, 0);
  }
  *BufferPtr = '\0';
  return InputBuffer;
}




#endif /* IO_H */
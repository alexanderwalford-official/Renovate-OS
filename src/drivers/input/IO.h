/*
    IO.h
    Alexander Walford 2023
*/

#ifndef IO_H
#define IO_H

#include "stdint.h"
#include "../video/VGA_linear.h"

// use uint16_t (16 bit unsigned integer) for 16bit keyboard signals

// experimental methods:

int InputBufferBusy = 0;
char* InputBuffer;

// get user input char and parse it to the input buffer, will be saved if the buffer state is 1
char GetChar() {
  uint16_t in;
  __asm__ __volatile__ ("int $0x16\n\t"
    : "=a"(in)
    : "0"(0x0));
  puts(0, 20, BLACK, BRIGHT, InputBufferBusy);
  HandleInputbuffer((char)in);
  return ((char)in);
}

// parse either 1 or 0 to enable to disable input buffer for current string
void InputBufferChangeState (int state) {
  InputBufferBusy = state;
}

void HandleInputbuffer(char in) {
  if (InputBuffer == 0) {
    InputBuffer += in;
  }
}

// formulate a string from the GetChar method
char *GetString(char *InputBuffer, int MaxChars) {
  char *BufferPtr = InputBuffer;
  while(BufferPtr < (InputBuffer + MaxChars) && (*BufferPtr == GetChar()) != '\r') {
    //FUNCT(*BufferPtr++, 0);
  }
  *BufferPtr = '\0';
  return InputBuffer;
}


#endif /* IO_H */
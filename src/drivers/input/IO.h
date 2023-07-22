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
char GetChar()
{
	uint16_t ax = 0;
	__asm__ ("int $0x16" : "+a" (ax));
  //HandleInputbuffer((char)ax);
	return ax & 0xff;
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
  InputBuffer = "";
  return;
}

void HandleInputbuffer(char in) {
  if (InputBuffer == 0) {
    InputBuffer += in;
  }
  return;
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
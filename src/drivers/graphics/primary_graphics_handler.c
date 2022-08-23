// primary_graphics_handler.cpp
// Alexander Walford 2022

/*
    This file will be used to design the screen refresh rate and
    will render updated positions for objects located on the screen.
*/


#include <stdio.h>
#include <stdlib.h>

int main () {
    ClockCycle(20);
    return 0;
}

void ClockCycle (int freq) {
    wait(freq);
    RenderScreen();
}

void RenderScreen() {
    // update screen components here

}
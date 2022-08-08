// primary_graphics_handler.cpp
// Alexander Walford 2022

/*
    This file will be used to design the screen refresh rate and
    will render updated positions for objects located on the screen.
*/


#include <iostream>
#include <chrono>
#include <thread>

using namespace std::this_thread; // sleep_for, sleep_until
using namespace std::chrono; // nanoseconds, system_clock, seconds

int main () {
    clockcycle(60);
    return 0;
}

int clockcycle(int fps) {
    sleep_for(nanoseconds(fps) * 10); // sleep for 600 milliseconds
    sleep_until(system_clock::now() + seconds(1));
    updatecomponents();
    clockcycle(fps);
    return 0;
}

int updatecomponents () {
    return 0;
}
// kernel.cpp
// Alexander Walford 2022


#include <iostream>
#include "header.h"

/*
    Primary Kernel File
    Handles:
    - Calling specific methods from other class files.
    - Core functionality of the operating system.
*/


int main () {
    std :: cout << ".." << std :: endl;

    // access boot class main function through header
    boot_main();

    return 0;
}

// method for clearing the screen
int c_s_cntr = 0;
int ClearScreen () {
    if (c_s_cntr < 200) {
        std :: cout << "" << std :: endl;
        c_s_cntr++;
        ClearScreen();
    }
}
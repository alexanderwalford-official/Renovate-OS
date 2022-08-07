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

    // access boot class through header
    boot_main();

    return 0;
}

int ClearScreen () {
    std :: cout << "" << std :: endl;
}
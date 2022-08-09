// kernel.cpp
// Alexander Walford 2022


#include <iostream>
#include "header.h"
#include <limits>

/*
    Primary Kernel File
    Handles:
    - Calling specific methods from other class files.
    - Core functionality of the operating system.
*/

// external asm call
extern "C" int PrimaryHandler();

// method for clearing the screen
int c_s_cntr = 0;
int ClearScreen() {
    if (c_s_cntr < 200) {
        std::cout << "" << std::endl;
        c_s_cntr++;
        ClearScreen();
    }
    return 0;
}

int LaunchMode () {
    std :: string input;
    std :: cout << "" << std :: endl;
    std :: cout << "" << std :: endl;
    std :: cout << "Renovate OS" << std :: endl;
    std :: cout << "Renovate Software LTD 2022" << std :: endl;
    std :: cout << "" << std :: endl;
    std :: cout << "" << std :: endl;
    std :: cout << "Please select an option from below:" << std :: endl;
    std :: cout << "" << std :: endl;
    std :: cout << "0 - Try Renovate OS" << std :: endl;
    std :: cout << "1 - Launch Installer" << std :: endl;
    std :: cout << "2 - System Recovery" << std :: endl;
    std :: cout << "" << std :: endl;
    std :: cout << "" << std :: endl;
    std :: cin >> input;

    if (input == "0") {
        // try Renovate OS
        std::cout << "SELECTED TRY MODE" << std :: endl;
    }
    else if (input == "1") {
        // Launch Installer
        std::cout << "SELECTED INSTALLER" << std::endl;
    }
    else if (input == "2") {
        // System recovery
        std::cout << "SELECTED RECOVERY MODE" << std::endl;
    }
    else {
        // System recovery
        std::cout << "ERROR: Please enter a valid menu option!" << std::endl;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        ClearScreen();
        LaunchMode();
    }

    return 0;
}

int HandleCommand () {
    return 0;
}

int ProccessStack () {
    return 0;
}

int main() {
    std::cout << ".." << std::endl;

    // access boot class main function through header
    boot_main();

    // ask the user how they'd like to launch the OS
    LaunchMode();

    return 0;
}
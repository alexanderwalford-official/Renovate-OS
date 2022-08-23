#include <stdio.h>

/*
    Primary Kernel File
    Handles:
    - Calling specific methods from other class files.
    - Core functionality of the operating system.
*/

// external asm call
extern PrimaryHandler();


int LaunchMode () {
    char input[] = "";
    printf("\n");
    printf("\n");
    printf("Renovate OS\n");
    printf("Renovate Software LTD 2022\n");
    printf("\n");
    printf("\n");
    printf("Please select an option from below:\n");
    printf("\n");
    printf("0 - Try Renovate OS\n");
    printf("0 - Launch Installer\n");
    printf("0 - System Recovery\n");
    printf("\n");
    printf("\n");
    scanf("%s", input);

    if (input == "0") {
        // try Renovate OS
        printf("Selected TRY mode.\n");
    }
    else if (input == "1") {
        // Launch Installer
        printf("Selected installer.\n");
    }
    else if (input == "2") {
        // System recovery
        printf("Selected recovery mode.\n");
    }
    else {
        printf("ERROR: Please enter a valid option!\n");
        getchar();
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
    printf("..\n");

    // access boot class main function through header
    boot_main();

    // ask the user how they'd like to launch the OS
    LaunchMode();

    return 0;
}
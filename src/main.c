#include "stdint.h"
#include "stdio.h"
#include "put.h"
#include "processmanagment.h"
#include "asm/primary_handler.asm"

// external ASM reference
extern PrimaryHandler();

int main() {
    clear(BRIGHT);
    // COL : ROW
    puts(35, 10, BLACK, BRIGHT, "Renovate OS");
    puts(28, 11, BLACK, BRIGHT, "Renovate Software LTD 2022");
    // p_create("puts(Renovate OS)"); // this is where we would print the text above
    // p_freeze(0, 4, 0); // this is where we could freeze the boot screen process before we load the main methods
    return 0;
}

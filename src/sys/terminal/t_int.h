// terminal interface

#ifndef T_INT_H
#define T_INT_H

#include "stdint.h"
#include "../../drivers/video/VGA_linear.h"

int cli_init (int ent) {

    if (ent == 0) {
        // bypass login
        cli_rend_main();
    }

    return 0;
}

void cli_rend_main () {

}

#endif /* T_INT_H */
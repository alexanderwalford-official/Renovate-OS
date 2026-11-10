// The C decoders AerOS uses (minimp3 and pl_mpeg) include <string.h>. The kernel has no C
// library, so they get AerOS's own instead: the Makefile adds this folder to the include path
// for lib/mp3.c and lib/mpg.c only
#include "../aer_string.h"

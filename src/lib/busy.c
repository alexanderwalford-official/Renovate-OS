/*
    busy.c - the hook behind busy() (see busy.h)
*/

#include "busy.h"

// the one real definition of the hook that busy.h declares. It starts as 0 (NULL), so busy()
// does nothing until a spinner is switched on. It has to live in a .c file, because a variable
// defined in the header would be a separate copy in every file that includes it
void (*busy_hook)(void) = 0;

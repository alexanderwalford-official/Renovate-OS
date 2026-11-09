/*
    busy.c - the hook behind busy() (see busy.h)
*/

#include "busy.h"

void (*busy_hook)(void) = 0;

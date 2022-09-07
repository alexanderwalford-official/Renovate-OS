#include "stdint.h"
#include "stdbool.h"

// "for each" loop
#define foreach(item, array) \
    for(int keep = 1, \
            count = 0,\
            size = sizeof (array) / sizeof *(array); \
        keep && count != size; \
        keep = !keep, count++) \
      for(item = (array) + count; keep; keep = !keep)

// dynamic vars
char str_chars[1];

// "conains" method
bool contains(char* string, char* target, int c) {
    if (c == 0) {
        str_chars[sizeof(string)]; // create an array, length of chars in the string
    }
    if (c < sizeof(string)) {
        str_chars[c] = string[c]; // set the values of the array to that of what is in the string
        c = c + 1;
        contains(string, target, c);
    }
    else {
        // now continue, the array contains all the chars from the string
        int cc = 0;
        int lc = 0;
        int mc = sizeof(target);
        for (lc = 0; lc < mc; ++lc) {
            (str_chars[lc] == target[lc]) ? cc = cc + 1 : 0;
        }
        if (cc == mc) {
            return true;
        }
        else {
            return false;
        }
    }
}

// "split" method
char* split(char* string, char split) {
    // TBI
}
#include "stdint.h"
#include "stdbool.h"

#ifndef NULL
#define NULL 0;
#endif

// "for each" loop
#define foreach(item, array) \
    for(int keep = 1, \
            count = 0,\
            size = sizeof (array) / sizeof *(array); \
        keep && count != size; \
        keep = !keep, count++) \
      for(item = (array) + count; keep; keep = !keep)



#ifndef SYS_PRIMARY_DEFINITIONS_H
#define SYS_PRIMARY_DEFINITIONS_H

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
    int s_loc = 0;
    char* new_string[sizeof(string) / 2][sizeof(string) / 2 - 1];
    for (int lc2 = 0; lc2 < sizeof(string); ++lc2) {
        if (string[lc2] == split) {
            // we found the character we're lookikng for the the string!
            s_loc = lc2;
            // add the rest of the characters to the 2nd dimension of the array
            for (int lc3 = lc2 + 1; lc3 < sizeof(string) - s_loc; ++lc3) {
                new_string[0][lc3] =  string[lc3];
            }
            // we're done, return the multi-dimensional array
            return new_string;
        }
        else {
            // add each char to new string before target
            new_string[lc2][0] = string[lc2];
        }
    }
}

// return to entry point
void ret () {
    return;
}
#endif /* SYS_PRIMARY_DEFINITIONS_H */
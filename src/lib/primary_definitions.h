#include "stdint.h"
#include "stdbool.h"
#include "../drivers/video/IO.h"

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
char* str_chars[];

// "conains" method
bool contains_str(char* string, char* target) {
    int cc = 0;
    int s_len_tar = sizeof(target);
    for (int lc2 = 0; lc2 < sizeof(string); ++lc2) {
        if (string[lc2] == target[lc2]) {
            // we found a character we're lookikng for the the string!
            cc = cc + 1;
        }
    }
    if (cc == sizeof(target)) {
        return true;
    }
    else {
        return false;
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

// "remove" method - seems to be returning an S.. and only running for 4 interations ..?
char* remove(char* string, char* target, int c) {
    int s_len_tar = sizeof(target);
    char* new_string[sizeof(string) - s_len_tar];

    // iterate through every character in the string array
    for (int lc2 = 0; lc2 < sizeof(string); ++lc2) {
        puts(lc2, 7, BLACK, BRIGHT, "+");
        if (string[lc2] == target[lc2]) {
            puts(0, 4, BLACK, BRIGHT, "MATCHES!");
            // we found a character we're lookikng for the the string!
            // add each character to the new string, from the s_loc onwards
            for (int lc3 = lc2 + s_len_tar; lc3 < sizeof(string); ++lc3) {
                new_string[lc3] =  string[lc3];
            }

        }
    }
    // we're done, return the new string
    return new_string;
}

// return to entry point
void ret () {
    return;
}
#endif /* SYS_PRIMARY_DEFINITIONS_H */
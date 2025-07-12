#include "stdint.h"
#include "stdbool.h"
#include "../drivers/video/VGA_linear.h"

#ifndef PI
#define PI 3.14159
#endif

#ifndef NULL
#define NULL 0
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

char buffer_str[1024]; //max buffer size

char *StrCombine(const char *str1, const char *str2) {
    // count length of str1
    int len1 = 0;
    while (str1[len1] != '\0') {
        len1++;
    }

    // count length of str2
    int len2 = 0;
    while (str2[len2] != '\0') {
        len2++;
    }

    // ensure the buffer is large enough
    if (len1 + len2 + 1 > sizeof(buffer_str)) {
        return NULL; // Buffer overflow, return NULL to indicate error
    }

    // copy str1 into buffer
    int i = 0;
    while (str1[i] != '\0') {
        buffer_str[i] = str1[i];
        i++;
    }

    // copy str2 into buffer
    int j = 0;
    while (str2[j] != '\0') {
        buffer_str[i + j] = str2[j];
        j++;
    }

    // add null terminator
    buffer_str[i + j] = '\0';

    return buffer_str;
}

char buffer[100]; //max buffer size

char *toArray(int number) {
    // handle the case where the number is 0
    if (number == 0) {
        buffer[0] = '0';
        return buffer;
    }

    // count the number of digits
    int n = 0;
    int temp = number;
    while (temp > 0) {
        temp /= 10;
        n++;
    }

    // fill the buffer with digits
    for (int i = n - 1; i >= 0; --i) {
        buffer[i] = (number % 10) + '0';
        number /= 10;
    }

    return buffer;
}


// compare if two strings are the same, used in conditional statements
int StrCompare (char* string_a, char* string_b) {
    
    // ensure that neither strings are NULL
    if (string_a == NULL || string_b == NULL) {
        return 0;
    }

    // calculate the length of both strings using string literals
    int len_a = 0;
    while (string_a[len_a] != '\0') {
        len_a++;
    }
    int len_b = 0;
    while (string_b[len_b] != '\0') {
        len_b++;
    }

    // check to see if the length is 0
    if (len_a == 0 || len_b == 0) {
        return 0;
    }

    // if the lengths are not the same, return false
    if (len_a != len_b) {
        return 0;
    }

    // compare each character in the strings
    for (int i = 0; i < len_a; i++) {
        //if (string_a[i] == '\0' || string_b[i] == '\0') break;
        if (string_a[i] != string_b[i]) {
            return 0;
        }
    }

    // if we reach here, the strings are the same
    return 1;
}

// many of these methods likely don't work

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
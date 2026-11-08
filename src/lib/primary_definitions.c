/*
    primary_definitions.c
    The Renovate specific string helpers.
*/
#include "primary_definitions.h"

// "contains" method, true if target appears anywhere inside string
bool contains_str(const char* string, const char* target) {
    if (string == NULL || target == NULL) {
        return false;
    }
    return strstr(string, target) != NULL;
}

static char StrCombine_buffer[1024]; //max buffer size

// join two strings together
// strings that do not fit are cut off rather than overflowing the buffer
char* StrCombine(const char* str1, const char* str2) {
    char combined[sizeof(StrCombine_buffer)];
    if (str1 == NULL) str1 = "";
    if (str2 == NULL) str2 = "";

    // build in a temporary buffer first, so StrCombine(StrCombine(a, b), c) works
    strlcpy(combined, str1, sizeof(combined));
    strlcat(combined, str2, sizeof(combined));

    strlcpy(StrCombine_buffer, combined, sizeof(StrCombine_buffer));
    return StrCombine_buffer;
}

static char toArray_buffer[16]; // big enough for "-2147483648"

// convert a number to a string
char* toArray(int number) {
    return itoa(number, toArray_buffer, 10);
}

// compare if two strings are the same, used in conditional statements
int StrCompare(const char* string_a, const char* string_b) {
    // ensure that neither strings are NULL
    if (string_a == NULL || string_b == NULL) {
        return 0;
    }
    return strcmp(string_a, string_b) == 0;
}

// "split" method
// splits string in place on the split character and stores a pointer to each part in parts
// at most max_parts parts are made, the last part keeps the rest of the string (like Python's split(sep, max))
// returns the number of parts, e.g. split("sd /sys/", ' ', parts, 2) -> parts = {"sd", "/sys/"}, returns 2
int split(char* string, char split_char, char** parts, int max_parts) {
    if (string == NULL || parts == NULL || max_parts <= 0) {
        return 0;
    }
    int count = 0;
    parts[count++] = string;
    for (char* p = string; *p && count < max_parts; ++p) {
        if (*p == split_char) {
            *p = '\0';
            parts[count++] = p + 1;
        }
    }
    return count;
}

// "remove" method, removes target from string in place
// c = the maximum number of times to remove it (0 or less removes every occurrence)
// returns string
char* StrRemove(char* string, const char* target, int c) {
    if (string == NULL || target == NULL || *target == '\0') {
        return string;
    }
    size_t target_len = strlen(target);
    int removed = 0;
    char* found = string;
    while ((c <= 0 || removed < c) && (found = strstr(found, target)) != NULL) {
        memmove(found, found + target_len, strlen(found + target_len) + 1);
        removed++;
    }
    return string;
}

// trim leading and trailing whitespace in place, returns the start of the trimmed string
char* StrTrim(char* string) {
    if (string == NULL) {
        return string;
    }
    while (isspace((unsigned char)*string)) {
        string++;
    }
    size_t len = strlen(string);
    while (len > 0 && isspace((unsigned char)string[len - 1])) {
        string[--len] = '\0';
    }
    return string;
}

// return to entry point
void ret(void) {
    return;
}

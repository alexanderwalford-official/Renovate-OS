/*
    primary_definitions.c
    The AerOS specific string helpers.

    These are small conveniences built on top of ren_string.c, mostly used by the terminal
    (t_int.c) to pick apart what the user typed, and by RenLang (ren_comp.c) to match keywords.
    They are written to be forgiving: a NULL string is treated as "nothing" instead of
    crashing, which suits code that deals with whatever the user happened to type.

    Two kinds of helper to be aware of:
    - in place helpers (split, StrRemove, StrTrim) change the string you give them, so it must
      be writable memory: a char array or a malloc'd buffer, never a string literal like "abc".
    - shared buffer helpers (StrCombine, toArray) hand back a pointer to one static buffer that
      the NEXT call overwrites. Use the result straight away, or copy it (strdup / strlcpy) if
      you need to keep it.
*/
#include "primary_definitions.h"

// "contains" method, true if target appears anywhere inside string
// just a NULL safe wrapper round strstr; an empty target counts as found (strstr finds "" at the start)
bool contains_str(const char* string, const char* target) {
    if (string == NULL || target == NULL) {
        return false;
    }
    return strstr(string, target) != NULL;
}

// the one buffer every StrCombine call returns: 1023 characters plus the '\0'
// it's static, so it lives on after the function returns (a local array would vanish)
static char StrCombine_buffer[1024]; //max buffer size

// join two strings together
// strings that do not fit are cut off rather than overflowing the buffer
// returns StrCombine_buffer, which the next call to StrCombine overwrites
char* StrCombine(const char* str1, const char* str2) {
    // a scratch copy on the stack (1KB of the kernel's 32KB stack, only while we're in here)
    char combined[sizeof(StrCombine_buffer)];
    // treat NULL as an empty string so callers don't need to check
    if (str1 == NULL) str1 = "";
    if (str2 == NULL) str2 = "";

    // build in a temporary buffer first, so StrCombine(StrCombine(a, b), c) works
    // the problem it solves: in a nested call one of the inputs IS StrCombine_buffer. If we wrote
    // straight into StrCombine_buffer, then StrCombine(a, StrCombine(b, c)) would overwrite "bc"
    // with a before we got round to appending it. Building the result elsewhere first means the
    // inputs are only read, never written, until the very end.
    // strlcpy / strlcat do the cutting off for us and always leave the result terminated
    strlcpy(combined, str1, sizeof(combined));
    strlcat(combined, str2, sizeof(combined));

    // now it's safe to replace the shared buffer
    strlcpy(StrCombine_buffer, combined, sizeof(StrCombine_buffer));
    return StrCombine_buffer;
}

static char toArray_buffer[16]; // big enough for "-2147483648"

// convert a number to a string
// (despite the name it gives back text, the "array" being the array of characters)
// returns toArray_buffer, which the next call overwrites
char* toArray(int number) {
    return itoa(number, toArray_buffer, 10);
}

// compare if two strings are the same, used in conditional statements
// returns 1 for a match and 0 otherwise, i.e. it answers "are these equal?"
// (strcmp answers "which comes first?", and gives 0 for a match, which is easy to get backwards)
int StrCompare(const char* string_a, const char* string_b) {
    // ensure that neither strings are NULL
    // (two NULLs count as "not the same", NULL isn't a string at all)
    if (string_a == NULL || string_b == NULL) {
        return 0;
    }
    return strcmp(string_a, string_b) == 0;
}

// "split" method
// splits string in place on the split character and stores a pointer to each part in parts
// at most max_parts parts are made, the last part keeps the rest of the string (like Python's split(sep, max))
// returns the number of parts, e.g. split("sd /sys/", ' ', parts, 2) -> parts = {"sd", "/sys/"}, returns 2
// how it works: nothing is copied. Each separator is overwritten with a '\0', which ends the
// previous part, and the next part starts on the character after it:
//     "sd /sys/"  becomes  "sd\0/sys/"   with parts[0] -> "sd", parts[1] -> "/sys/"
// gotchas:
// - the string is changed, so pass a writable copy (not a literal) if you still need the original
// - two separators in a row give an empty part between them ("a  b" -> "a", "", "b")
// - parts must have room for max_parts pointers
// the terminal calls this with max_parts = 2 to separate a command from its arguments
int split(char* string, char split_char, char** parts, int max_parts) {
    if (string == NULL || parts == NULL || max_parts <= 0) {
        return 0;
    }
    int count = 0;
    // the first part always starts at the beginning, even if there's no separator at all
    parts[count++] = string;
    // stop once we have max_parts parts: any separators after that stay in the last part
    for (char* p = string; *p && count < max_parts; ++p) {
        if (*p == split_char) {
            // end the current part here and start the next one just after the separator
            *p = '\0';
            parts[count++] = p + 1;
        }
    }
    return count;
}

// "remove" method, removes target from string in place
// c = the maximum number of times to remove it (0 or less removes every occurrence)
// returns string
// e.g. StrRemove(s, "ab", 0) on "xabyab" leaves "xy", StrRemove(s, "ab", 1) leaves "xyab"
// each search carries on from where the last removal happened, so text that only lines up into a
// new match because of a removal is left alone: removing "ab" from "aabb" gives "ab" (like Python's
// "aabb".replace("ab", ""))
char* StrRemove(char* string, const char* target, int c) {
    // an empty target would "match" everywhere and loop forever, so there is nothing to do
    if (string == NULL || target == NULL || *target == '\0') {
        return string;
    }
    size_t target_len = strlen(target);
    int removed = 0;
    char* found = string;
    // keep going while we're under the limit (or there is no limit) and another match turns up
    while ((c <= 0 || removed < c) && (found = strstr(found, target)) != NULL) {
        // slide everything after the match left over the top of it, closing the gap
        // the + 1 brings the '\0' along too; memmove (not memcpy) because the two regions overlap
        memmove(found, found + target_len, strlen(found + target_len) + 1);
        removed++;
    }
    return string;
}

// trim leading and trailing whitespace in place, returns the start of the trimmed string
// the leading whitespace is skipped rather than shifted, so the returned pointer may be further along
// than the one you passed in. Use the returned pointer, but if the string came from malloc,
// free() the ORIGINAL pointer, not the trimmed one.
// e.g. "  ls /sys  " -> returns a pointer to "ls /sys"
char* StrTrim(char* string) {
    if (string == NULL) {
        return string;
    }
    // step over leading spaces, tabs, newlines...
    while (isspace((unsigned char)*string)) {
        string++;
    }
    // then chop trailing whitespace off by writing a new '\0' over each one, working backwards
    // (len > 0 stops us running off the front when the string was all whitespace)
    size_t len = strlen(string);
    while (len > 0 && isspace((unsigned char)string[len - 1])) {
        string[--len] = '\0';
    }
    return string;
}

// return to entry point
// in practice this does nothing at all: it just returns to whoever called it. It's an old
// placeholder that nothing uses any more, and since the kernel is linked with --gc-sections
// an unused function like this is left out of the final kernel image anyway.
void ret(void) {
    return;
}

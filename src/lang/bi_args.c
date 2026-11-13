/*
    bi_args.c
    Checking and unpacking the arguments of AerLang's built-in functions.

    A built-in gets its arguments as an array of values, and any of them might be the wrong
    kind: a program can pass a string where a number belongs. Each helper here checks one
    argument and hands it back in the form the built-in wants, or stops the program with a
    message naming the function and the argument (aer_error never returns, so the built-in can
    trust what it gets back). The ones for files also check the program may use the file, asking
    the person at the keyboard first if it needs to (sys/perm.h).

    How a built-in works, and how to add one, is at the top of builtins.h.
*/

#include "builtins.h"
#include "../lib/aer_math.h"
#include "../sys/f_sys.h"

// a number argument, as a whole number. A decimal is rounded down (towards minus infinity), so
// a game can keep its positions as decimals and still draw at rect(x, y, ...)
int32_t int_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type == V_FLOAT) {
        double whole = floor(argv[i].as.f);
        // the limits are one past each end of a 32 bit whole number (-2147483648 to 2147483647),
        // so anything that fits passes. Writing it as !(inside) rather than (outside) also
        // catches nan, which fails every comparison
        if (!(whole > -2147483649.0 && whole < 2147483648.0)) {
            aer_error(line, "%s() argument %d is too big for a whole number", fn, i + 1);
        }
        return (int32_t)whole;
    }
    // anything else that isn't a whole number (a string, a list, nil...) is a mistake in the program
    if (argv[i].type != V_INT) {
        aer_error(line, "%s() needs a number for argument %d, not a %s", fn, i + 1, aer_type_name(argv[i]));
    }
    return argv[i].as.i;
}

// a number argument, whole or decimal, as a decimal
double real_arg (value_t* argv, int i, int line, const char* fn) {
    if (!aer_is_number(argv[i])) {
        aer_error(line, "%s() needs a number for argument %d, not a %s", fn, i + 1, aer_type_name(argv[i]));
    }
    return aer_real(argv[i]);
}

// a string argument, as the string object itself. It is only borrowed: the caller still owns
// it, so don't aer_release() it, and aer_retain() it if it has to be kept after returning
aer_object_t* str_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type != V_STR) {
        aer_error(line, "%s() needs a string for argument %d, not a %s", fn, i + 1, aer_type_name(argv[i]));
    }
    return argv[i].as.obj;
}

// a list argument, as the list object itself (borrowed, the same as str_arg). Changing the
// list changes the program's own list, which is how push(), pop() and friends work
aer_object_t* list_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type != V_LIST) {
        aer_error(line, "%s() needs a list for argument %d, not a %s", fn, i + 1, aer_type_name(argv[i]));
    }
    return argv[i].as.obj;
}

// a bytes argument (a picture or a sound), as the object itself (borrowed, the same as str_arg)
aer_object_t* bytes_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type != V_BYTES) {
        aer_error(line, "%s() needs bytes (from rdb) for argument %d, not a %s", fn, i + 1, aer_type_name(argv[i]));
    }
    return argv[i].as.obj;
}

// Split a file's path, given to a function as argument i, into its folder and name, and check
// the program may read (PERM_READ) or change (PERM_CHANGE) it: if it needs to, AerOS asks the
// person at the keyboard first (sys/perm.h). False if the path is no good or the answer is no,
// and then the function fails the same way as for a file that isn't there (nil, or false)
bool file_arg (value_t* argv, int i, int line, const char* fn, perm_kind_t kind, char* dir, char* name) {
    aer_object_t* path = str_arg(argv, i, line, fn);
    // && stops at the first false, so a path that can't be split never gets as far as asking
    return fs_split_path(path->data.chars, dir, FS_PATH_MAX, name, FS_NAME_MAX)
           && perm_allows(dir, name, kind);
}

// the same for a function that wants the path itself: the path, or NULL if it is no good or
// the program isn't allowed to use it
const char* allowed_path (value_t* argv, int i, int line, const char* fn, perm_kind_t kind) {
    // the folder and name are only needed for the permission check, so they live here
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    const char* path = str_arg(argv, i, line, fn)->data.chars;
    bool allowed = fs_split_path(path, dir, sizeof(dir), name, sizeof(name)) && perm_allows(dir, name, kind);
    return allowed ? path : NULL;
}

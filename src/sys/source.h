/*
    source.h - AerOS's own source code, built into AerOS itself

    One idea behind AerOS is that you can find out how anything in it works without leaving it.
    So the source code it was built from is linked into the kernel (the Makefile does that, see
    SOURCE_CODE there), and shows up as a folder you can read but not change:

        la /src/sys/            the files
        read /src/sys/fat.c     one in the reader
        src del                 the code that runs a command (see WHY and SRC in t_int.c)

    The files are the exact ones this copy of AerOS was built from, so they always match what
    is running. Left out: other people's code (lib/pl_mpeg, lib/minimp3) and two files that are
    only data (the splash screen's logo and the console font).

    f_sys.c hands every path under /src/ to the functions here, so every command that reads
    files works on /src/ too. Anything that would change a file there gets FS_ERR_READ_ONLY.
*/
#ifndef SOURCE_H
#define SOURCE_H

#include <stdint.h>
#include <stdbool.h>
#include "fat.h"   // fs_list_fn

#define SOURCE_FOLDER "/src/"

// one built-in source file. path is relative to the source folder: "sys/fat.c", "Makefile"
typedef struct {
    const char* path;
    const char* start;   // its bytes, from start up to (not including) end
    const char* end;
} source_file_t;

// the table the Makefile writes (obj/source_index.c)
extern const source_file_t source_files[];
extern const int source_file_count;

// is this absolute folder path inside /src/ ("/src/" itself, "/src/sys/"...)?
bool source_in (const char* folder);

// find a file: folder is an absolute path inside /src/, name a file name. NULL if there's none.
// Upper and lower case don't matter, like on a FAT disk
const source_file_t* source_find (const char* folder, const char* name);

// does this folder exist? ("/src/" always does; "/src/sys/" does because files are in it)
bool source_folder_exists (const char* folder);

// call fn for the files and folders directly inside a folder, returns how many (-1 if none)
int source_list (const char* folder, fs_list_fn fn, void* ctx);

// print a folder for LA, returns how many lines
int source_print (const char* folder);

// a file's length in bytes
uint32_t source_size (const source_file_t* file);

// look for a source file whose path ends with `ending` ("fat.c", "sys/fat.c"): the first match.
// Handy for SRC, where typing the whole path would be tedious
const source_file_t* source_search (const char* ending);


// ---- Reading the C code (for WHY and SRC, see sys/explain.c) ------------------------------------
//
// These rely on AerOS's code style rather than understanding C properly, which keeps them
// short: a function's definition is the one line that starts at column 0 with its return type
// and name, like
//
//     int fs_list (const char* d_path, fs_list_fn fn, void* ctx) {
//
// its body ends at the first line that starts with "}", and the comment that explains it is
// right above it, with no blank line in between.

// a line in a source file
typedef struct {
    const source_file_t* file;
    int line;                  // counting from 1
} source_place_t;

// where a C function is defined, looking through every .c file. False if AerOS has none
bool source_find_function (const char* function, source_place_t* place);

// where a .h file declares a function (a ';' comes before any '{'). The comment above a
// declaration is often the best description of what a function is for
bool source_find_declaration (const char* function, source_place_t* place);

// line `line` of a file: where it starts, and its length without the '\n' (or "\r\n") on the
// end. NULL if the file is shorter than that
const char* source_line (const source_file_t* file, int line, size_t* length);

// the last line of the function whose definition starts on `line`: the next "}" at column 0
int source_function_end (const source_file_t* file, int line);

// the first line of the comment right above a line (`line` itself if there is no comment)
int source_comment_start (const source_file_t* file, int line);

// the comment right above a line, without its "//" or "/* */" marks and indentation, one
// comment line per text line. Writes "" when there is no comment there. Returns its length
size_t source_comment_above (const source_file_t* file, int line, char* out, size_t size);

#endif /* SOURCE_H */

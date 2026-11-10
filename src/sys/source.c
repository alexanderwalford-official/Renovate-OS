/*
    source.c - the read-only /src/ folder: AerOS's own source code (see source.h)

    The Makefile links every source file into the kernel with "ld -b binary" (like the built-in
    apps, see sys/apps.c) and writes a table of them, source_files[], into obj/source_index.c:

        { "sys/fat.c", _binary_sys_fat_c_start, _binary_sys_fat_c_end },
        { "sys/terminal/t_int.c", ... },

    There is no real folder anywhere. A folder like /src/sys/ "exists" because some file's path
    starts with "sys/", and listing it means looking through the table for the paths directly
    inside it. The table is small (about 100 files), so looking through all of it is quick.
*/

#include "source.h"
#include "f_sys.h"
#include "../lib/primary_definitions.h"

#define SOURCE_FOLDER_LENGTH 5 // "/src/"

bool source_in (const char* folder) {
    return strncasecmp(folder, SOURCE_FOLDER, SOURCE_FOLDER_LENGTH) == 0;
}

// the part of a /src/ folder path after "/src/": "/src/sys/" -> "sys/", "/src/" -> ""
static const char* inside (const char* folder) {
    return folder + SOURCE_FOLDER_LENGTH;
}

// does `path` start with `prefix`, ignoring upper / lower case?
static bool starts_with (const char* path, const char* prefix) {
    return strncasecmp(path, prefix, slen(prefix)) == 0;
}

uint32_t source_size (const source_file_t* file) {
    return (uint32_t)(file->end - file->start);
}

const source_file_t* source_find (const char* folder, const char* name) {
    char wanted[FS_PATH_MAX];
    sfmt(wanted, sizeof(wanted), "%s%s", inside(folder), name);
    for (int i = 0; i < source_file_count; i++) {
        if (strcasecmp(source_files[i].path, wanted) == 0) {
            return &source_files[i];
        }
    }
    return NULL;
}

bool source_folder_exists (const char* folder) {
    const char* prefix = inside(folder);
    if (prefix[0] == '\0') {
        return true; // /src/ itself
    }
    for (int i = 0; i < source_file_count; i++) {
        if (starts_with(source_files[i].path, prefix)) {
            return true;
        }
    }
    return false;
}

// For a file inside `prefix`, what comes next in its path: a file name, or the name of a folder
// it is further inside. "sys/terminal/t_int.c" inside "sys/" is the folder "terminal".
// Writes the name into `out` and returns true for a folder
static bool next_part (const char* path, const char* prefix, char* out, size_t size) {
    const char* rest = path + slen(prefix);
    const char* slash = strchr(rest, '/');
    if (slash == NULL) {
        scpy(out, rest, size);
        return false;
    }
    size_t length = (size_t)(slash - rest);
    if (length >= size) {
        length = size - 1;
    }
    memcpy(out, rest, length);
    out[length] = '\0';
    return true;
}

// has an earlier file (before index `before`) already shown the folder `name` inside `prefix`?
// (each folder must be listed once, however many files are in it)
static bool folder_listed_before (int before, const char* prefix, const char* name) {
    for (int i = 0; i < before; i++) {
        if (!starts_with(source_files[i].path, prefix)) {
            continue;
        }
        char other[FS_NAME_MAX];
        if (next_part(source_files[i].path, prefix, other, sizeof(other)) && strcasecmp(other, name) == 0) {
            return true;
        }
    }
    return false;
}

// one look through the table for what is directly inside `prefix`: only the folders
// (want_folders) or only the files. Returns how many fn was called for
static int list_pass (const char* prefix, bool want_folders, fs_list_fn fn, void* ctx) {
    int count = 0;
    for (int i = 0; i < source_file_count; i++) {
        const source_file_t* file = &source_files[i];
        if (!starts_with(file->path, prefix)) {
            continue;
        }
        char name[FS_NAME_MAX];
        bool is_folder = next_part(file->path, prefix, name, sizeof(name));
        if (is_folder != want_folders) {
            continue;
        }
        if (is_folder && folder_listed_before(i, prefix, name)) {
            continue;
        }
        fn(name, is_folder, is_folder ? 0 : source_size(file), ctx);
        count++;
    }
    return count;
}

int source_list (const char* folder, fs_list_fn fn, void* ctx) {
    if (!source_folder_exists(folder)) {
        return -1;
    }
    // folders first, then files, like every other folder listing
    const char* prefix = inside(folder);
    int folders = list_pass(prefix, true, fn, ctx);
    int files = list_pass(prefix, false, fn, ctx);
    return folders + files;
}

// print one entry the way LA shows them
static void print_entry (const char* name, bool is_dir, uint32_t size, void* ctx) {
    int* shown = ctx;
    if (is_dir) {
        prtf("  <DIR>   %s/\n", name);
    } else {
        prtf("  <FILE>  %-22s %6u bytes  (read only)\n", name, size);
    }
    (*shown)++;
}

int source_print (const char* folder) {
    int shown = 0;
    source_list(folder, print_entry, &shown);
    return shown;
}

const source_file_t* source_search (const char* ending) {
    size_t wanted = slen(ending);
    for (int i = 0; i < source_file_count; i++) {
        const char* path = source_files[i].path;
        size_t length = slen(path);
        if (length < wanted) {
            continue;
        }
        const char* tail = path + length - wanted;
        bool whole_name = tail == path || tail[-1] == '/'; // "fat.c" mustn't match "myfat.c"
        if (whole_name && strcasecmp(tail, ending) == 0) {
            return &source_files[i];
        }
    }
    return NULL;
}


// =================================================================================================
//  Reading the C code
// =================================================================================================

// is this character part of a C name? (letters, digits, '_')
static bool name_character (char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// does a path end with this extension (".c", ".h")?
static bool has_extension (const char* path, const char* extension) {
    size_t length = slen(path);
    size_t wanted = slen(extension);
    return length > wanted && seq(path + length - wanted, extension);
}

// the end of the line starting at `start`: the '\n' after it, or the end of the file
static const char* line_end (const source_file_t* file, const char* start) {
    const char* end = start;
    while (end < file->end && *end != '\n') {
        end++;
    }
    return end;
}

// Does this one line (start up to end) name `function` the way a definition or declaration
// does? That is: it starts at column 0 (not with a space, a '#' or a comment), and the name is
// in it as a whole word followed by '(' (spaces allowed)
static bool names_function (const char* start, const char* end, const char* function) {
    if (start == end || *start == ' ' || *start == '\t' || *start == '#' || *start == '/') {
        return false;
    }
    size_t name_length = slen(function);
    for (const char* p = start; p + name_length < end; p++) {
        bool whole_word = (p == start || !name_character(p[-1])) && !name_character(p[name_length]);
        if (!whole_word || strncmp(p, function, name_length) != 0) {
            continue;
        }
        const char* after = p + name_length;
        while (after < end && *after == ' ') {
            after++;
        }
        if (after < end && *after == '(') {
            return true;
        }
    }
    return false;
}

// Is the function named at `start` only declared there (a ';' comes before any '{'), rather
// than defined (its body, '{', comes first)? A declaration can carry on over a few lines:
//     void explain_say (problem_t problem, const char* subject, const char* format, ...)
//         __attribute__((format(printf, 3, 4)));
static bool is_declaration (const source_file_t* file, const char* start) {
    for (const char* p = start; p < file->end; p++) {
        if (*p == ';') {
            return true;
        }
        if (*p == '{') {
            return false;
        }
    }
    return false;
}

// look through every file with this extension for the line that names `function`, in a
// declaration or a definition
static bool find_line (const char* function, const char* extension, bool declaration, source_place_t* place) {
    for (int i = 0; i < source_file_count; i++) {
        const source_file_t* file = &source_files[i];
        if (!has_extension(file->path, extension)) {
            continue;
        }
        int line = 1;
        for (const char* start = file->start; start < file->end; line++) {
            const char* end = line_end(file, start);
            if (names_function(start, end, function) && is_declaration(file, start) == declaration) {
                place->file = file;
                place->line = line;
                return true;
            }
            start = end + 1;
        }
    }
    return false;
}

bool source_find_function (const char* function, source_place_t* place) {
    bool declaration = false;
    return find_line(function, ".c", declaration, place);
}

bool source_find_declaration (const char* function, source_place_t* place) {
    bool declaration = true;
    return find_line(function, ".h", declaration, place);
}

const char* source_line (const source_file_t* file, int line, size_t* length) {
    const char* start = file->start;
    for (int n = 1; n < line; n++) {
        start = line_end(file, start) + 1;
        if (start >= file->end) {
            return NULL;
        }
    }
    if (start >= file->end || line < 1) {
        return NULL;
    }
    const char* end = line_end(file, start);
    if (end > start && end[-1] == '\r') {
        end--;
    }
    *length = (size_t)(end - start);
    return start;
}

int source_function_end (const source_file_t* file, int line) {
    size_t length;
    for (int n = line + 1; ; n++) {
        const char* text = source_line(file, n, &length);
        if (text == NULL) {
            return n - 1; // ran off the end of the file: stop at its last line
        }
        if (length > 0 && text[0] == '}') {
            return n;
        }
    }
}

// is `what` somewhere in this line? (the line isn't a '\0' ended string, so strstr won't do)
static bool line_contains (const char* text, size_t length, const char* what) {
    size_t wanted = slen(what);
    for (size_t i = 0; i + wanted <= length; i++) {
        if (strncmp(text + i, what, wanted) == 0) {
            return true;
        }
    }
    return false;
}

// Which kind of comment line is this? A "//" line, the last line of a "/* */" block (it ends
// with "*/"), or neither. Leading spaces don't matter
typedef enum { NOT_A_COMMENT, LINE_COMMENT, BLOCK_COMMENT_END } comment_kind_t;

static comment_kind_t comment_kind (const char* text, size_t length) {
    size_t i = 0;
    while (i < length && text[i] == ' ') {
        i++;
    }
    if (i + 1 < length && text[i] == '/' && text[i + 1] == '/') {
        return LINE_COMMENT;
    }
    bool ends_block = length >= 2 && text[length - 2] == '*' && text[length - 1] == '/';
    return ends_block ? BLOCK_COMMENT_END : NOT_A_COMMENT;
}

// add one comment line to `out` without its marks: "// does X" -> "does X", "/* a */" -> "a"
static void add_comment_text (const char* text, size_t length, char* out, size_t size) {
    // drop the indentation and the marks at both ends
    while (length > 0 && *text == ' ') {
        text++;
        length--;
    }
    if (length >= 2 && (strncmp(text, "//", 2) == 0 || strncmp(text, "/*", 2) == 0)) {
        text += 2;
        length -= 2;
    }
    if (length >= 2 && strncmp(text + length - 2, "*/", 2) == 0) {
        length -= 2;
    }
    // and one space after "//", plus any trailing spaces
    if (length > 0 && *text == ' ') {
        text++;
        length--;
    }
    while (length > 0 && text[length - 1] == ' ') {
        length--;
    }

    size_t used = slen(out);
    bool first_line = used == 0;
    if (!first_line && used + 1 < size) {
        out[used++] = '\n';
    }
    if (used + length >= size) {
        length = size - used - 1;
    }
    memcpy(out + used, text, length);
    out[used + length] = '\0';
}

int source_comment_start (const source_file_t* file, int line) {
    size_t length;
    const char* text = source_line(file, line - 1, &length);
    if (text == NULL) {
        return line; // the first line of the file: nothing above it
    }

    if (comment_kind(text, length) == BLOCK_COMMENT_END) {
        // a /* */ block: go up to the line it opens on
        for (int opening = line - 1; opening >= 1; opening--) {
            text = source_line(file, opening, &length);
            if (line_contains(text, length, "/*")) {
                return opening;
            }
        }
        return line; // a "*/" with no "/*" above it: not a comment we understand
    }

    // "//" lines: go up while they carry on
    int first = line;
    while (first > 1) {
        text = source_line(file, first - 1, &length);
        if (comment_kind(text, length) != LINE_COMMENT) {
            break;
        }
        first--;
    }
    return first;
}

size_t source_comment_above (const source_file_t* file, int line, char* out, size_t size) {
    out[0] = '\0';
    size_t length;
    int first = source_comment_start(file, line);
    for (int n = first; n < line; n++) {
        const char* text = source_line(file, n, &length);
        add_comment_text(text, length, out, size);
    }
    return slen(out);
}

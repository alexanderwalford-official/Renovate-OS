/*
    f_sys.c - the file system everything else uses (see f_sys.h for the overview)

    Every command that touches a file comes through here. This file does three jobs:

    1. PATHS. People type things like "notes.txt", "../bin", "/sys/./cache" or "sys//tmp".
       Before anything else happens, those are all turned into one clean, absolute form
       ("/sys/cache/"), so neither the disk driver nor the memory tables ever have to deal
       with "." or "..".

    2. PICKING WHERE FILES GO. fs_init() runs once at boot and sets fs_mode:

           normal boot   look for a FAT disk labelled AEROS. Found one? DISK mode: every call
                         is handed on to the disk driver, sys/fat.c. No disk? VFS mode.
           demo boot     straight to VFS mode, without even looking at the disks

       After that, each public fs_ function does the same two steps: clean up the path, then
       call the disk version (fat_...) or the memory version (mem_...) of itself.

    3. THE TEMPORARY FILE SYSTEM (VFS mode, the mem_ functions). It is kept simple on purpose:
       two fixed size tables, one of files and one of folders. Each slot has a "used" flag, and
       things are found by looking through the table from the start. Folders aren't a tree,
       just a list of full paths ("/sys/cache/"), and a file remembers the path of the folder it
       is in. Only the contents of files are malloc'd. It is all gone when the computer restarts.

    https://wiki.osdev.org/VFS
*/
#include "f_sys.h"
#include "sys_clck.h"
#include "../lib/primary_definitions.h"
#include "../lib/busy.h"

int fs_mode = FS_MODE_MEMORY;  // VFS until fs_init() finds a disk
char fs_disk_status[96] = "";  // what happened when looking for a disk, e.g. "no AEROS disk found"
int f_c = 0;                   // how many files[] slots are in use (VFS mode)
int d_c = 0;                   // how many directories[] slots are in use (VFS mode, "/" counts)
char cd[FS_PATH_MAX] = "/";    // the current folder: always absolute and ending in '/'

// the next ID to hand out. IDs only ever count up, so a deleted file's ID is never used again
static int next_file_id = 1;
static int next_folder_id = 1;

// the VFS tables. A slot is free when its f_used / d_used flag is false
// (static arrays start out all zero, so every slot starts free)
static struct file files[FS_MAX_FILES];
static struct directory directories[FS_MAX_DIRS];

// is the disk in use (DISK mode), rather than the temporary file system?
static bool on_disk (void) {
    return fs_mode == FS_MODE_DISK;
}


// =================================================================================================
//  Paths
// =================================================================================================

// A valid file or folder name: not empty, not too long, no '/', not "." or "..", and only
// printable characters. (On a disk the FAT driver is stricter still: the name has to fit the
// 8.3 format, which fat_make_83() checks)
bool fs_valid_name (const char* name) {
    if (name == NULL || name[0] == '\0' || strlen(name) >= FS_NAME_MAX) {
        return false;
    }
    // "." and ".." mean "here" and "the folder above" in a path, so they can't be names
    if (StrCompare(name, ".") || StrCompare(name, "..")) {
        return false;
    }
    for (const char* c = name; *c != '\0'; c++) {
        // a '/' would split the name in two, and control characters would mess up the screen
        if (*c == '/' || !isprint((unsigned char)*c)) {
            return false;
        }
    }
    return true;
}

// change a folder path into its parent, in place: "/sys/cache/" becomes "/sys/".
// "/" stays "/" (there is nothing above it).
// How: drop the '/' at the end ("/sys/cache"), find the last '/' that is left (the one in
// front of "cache"), and end the string just after it
static void go_up_one_folder (char* path) {
    size_t length = strlen(path);
    if (length <= 1) {
        return; // "/" already
    }
    path[length - 1] = '\0';
    char* last_slash = strrchr(path, '/'); // never NULL: every path starts with '/'
    last_slash[1] = '\0';
}

// Turn a folder path as someone typed it (absolute, or relative to the current folder, maybe
// with "." and "..") into a clean absolute path ending in '/'. False if it would be too long.
// It only works out the path, it doesn't check the folder exists. With cd = "/sys/":
//
//     ""            ->  "/sys/"        (nothing means "here")
//     "cache"       ->  "/sys/cache/"
//     "../bin/"     ->  "/bin/"
//     "/a//b/./c"   ->  "/a/b/c/"      (repeated slashes and "." are ignored)
//     "/.."         ->  "/"            (you can't go above the top folder)
bool fs_resolve_dir (const char* input, char* out, size_t out_size) {
    if (input == NULL || input[0] == '\0') {
        input = ".";
    }

    // the path built up so far, always ending in '/'. A '/' at the start of the input means
    // start from the top, anything else starts from the current folder
    char path[FS_PATH_MAX];
    if (input[0] == '/') {
        strlcpy(path, "/", sizeof(path));
    } else {
        strlcpy(path, cd, sizeof(path));
    }

    // go through the input one part at a time ("sys", "..", "cache"...)
    const char* position = input;
    while (*position != '\0') {
        // skip the slash (or slashes) in front of the next part
        while (*position == '/') {
            position++;
        }
        if (*position == '\0') {
            break; // the input ended with a slash
        }

        // the part runs up to the next '/' or the end
        const char* part = position;
        size_t part_length = strcspn(position, "/");
        position += part_length;

        bool is_dot = part_length == 1 && part[0] == '.';
        bool is_dot_dot = part_length == 2 && part[0] == '.' && part[1] == '.';

        if (is_dot) {
            continue; // "." means stay here
        }
        if (is_dot_dot) {
            go_up_one_folder(path); // ".." means up one (and at "/", stay there)
            continue;
        }

        // an ordinary name: add "name/" on the end. That needs the name, the '/' and the '\0'
        size_t path_length = strlen(path);
        bool name_too_long = part_length >= FS_NAME_MAX;
        bool path_too_long = path_length + part_length + 2 > sizeof(path);
        if (name_too_long || path_too_long) {
            return false;
        }
        memcpy(path + path_length, part, part_length);
        path[path_length + part_length] = '/';
        path[path_length + part_length + 1] = '\0';
    }

    // strlcpy returns how long the string is; if that doesn't fit, `out` was too small
    return strlcpy(out, path, out_size) < out_size;
}

// Split a file path as someone typed it ("notes.txt", "/sys/conf.aer", "../a.txt") into the
// folder it is in (cleaned up like any folder path) and the file's name.
// Everything up to the last '/' is the folder, everything after it is the name. With no '/'
// at all, the file is in the current folder. False if the path is empty or too long, or the
// name isn't valid (e.g. "sys/" has no name after the '/')
bool fs_split_path (const char* input, char* dir_out, size_t dir_size, char* name_out, size_t name_size) {
    if (input == NULL || input[0] == '\0') {
        return false;
    }

    const char* last_slash = strrchr(input, '/');
    if (last_slash == NULL) {
        // just a name, e.g. "notes.txt"
        strlcpy(dir_out, cd, dir_size);
        strlcpy(name_out, input, name_size);
    } else {
        // copy out the folder part, e.g. "../sys/" from "../sys/a.txt", and clean it up
        char folder[FS_PATH_MAX];
        size_t folder_length = (size_t)(last_slash - input) + 1; // up to and including the '/'
        if (folder_length >= sizeof(folder)) {
            return false;
        }
        memcpy(folder, input, folder_length);
        folder[folder_length] = '\0';
        if (!fs_resolve_dir(folder, dir_out, dir_size)) {
            return false;
        }
        strlcpy(name_out, last_slash + 1, name_size);
    }
    return fs_valid_name(name_out);
}

// the folder above a folder: "/sys/cache/" -> "/sys/" ("/" is its own parent).
// d_path must already be a clean absolute path ending in '/'
void fs_parent_path (const char* d_path, char* out, size_t out_size) {
    strlcpy(out, d_path, out_size);
    go_up_one_folder(out);
}

// is `path` a folder directly inside `parent`? With parent "/sys/":
//     "/sys/cache/"       yes
//     "/sys/cache/old/"   no, that's inside a folder inside it
//     "/sys/"             no, that's the parent itself
static bool is_directly_inside (const char* parent, const char* path) {
    size_t parent_length = strlen(parent);
    size_t path_length = strlen(path);

    // it has to be longer than the parent, and start with the parent's path
    if (path_length <= parent_length || strncmp(path, parent, parent_length) != 0) {
        return false;
    }
    // what is left must be "name/": the first '/' after the parent's part has to be the very
    // last character
    const char* rest = path + parent_length;
    return strchr(rest, '/') == path + path_length - 1;
}


// =================================================================================================
//  The temporary file system (VFS mode)
// =================================================================================================
//
// All of these take clean absolute paths: the public fs_ functions further down tidy them up
// first.

// where the folder with exactly this path is in directories[], or -1 if there is none
static int mem_find_folder (const char* path) {
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (directories[i].d_used && StrCompare(directories[i].d_path, path)) {
            return i;
        }
    }
    return -1;
}

// where the file with this name in this folder is in files[], or -1 if there is none.
// (In the VFS names care about upper and lower case, unlike on a FAT disk)
static int mem_find_file (const char* folder, const char* name) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].f_used && StrCompare(files[i].f_path, folder) && StrCompare(files[i].f_name, name)) {
            return i;
        }
    }
    return -1;
}

// the name of a folder from its path, without the '/' on the end, written into `name`.
// `parent` is the path of the folder it is in: "/sys/cache/" in "/sys/" gives "cache"
static void folder_name (const char* path, const char* parent, char* name, size_t size) {
    strlcpy(name, path + strlen(parent), size);
    size_t length = strlen(name);
    if (length > 0 && name[length - 1] == '/') {
        name[length - 1] = '\0';
    }
}

// add a folder to the table. Its parent has to be there already (except for "/" itself).
// `description` is shown next to it by LA (it may be NULL).
// Returns FS_OK, FS_ERR_EXISTS, FS_ERR_NOT_FOUND (no parent) or FS_ERR_FULL (no free slot)
static int mem_create_folder (const char* path, const char* description) {
    if (mem_find_folder(path) >= 0) {
        return FS_ERR_EXISTS;
    }
    char parent[FS_PATH_MAX];
    fs_parent_path(path, parent, sizeof(parent));
    bool is_top = StrCompare(path, "/");
    if (!is_top && mem_find_folder(parent) < 0) {
        return FS_ERR_NOT_FOUND;
    }

    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (directories[i].d_used) {
            continue;
        }
        // the first free slot
        struct directory* folder = &directories[i];
        memset(folder, 0, sizeof(*folder)); // clear anything a deleted folder left behind
        folder->d_used = true;
        folder->d_id = next_folder_id;
        next_folder_id++;
        strlcpy(folder->d_path, path, sizeof(folder->d_path));
        strlcpy(folder->d_name, description != NULL ? description : "", sizeof(folder->d_name));
        d_c++;
        return FS_OK;
    }
    return FS_ERR_FULL;
}

// add a file to the table, holding a copy of `length` bytes of `contents`.
// Returns FS_OK, FS_ERR_NOT_FOUND (no such folder), FS_ERR_EXISTS, FS_ERR_NO_MEMORY or FS_ERR_FULL
static int mem_create_file (const char* folder, const char* name, const char* contents, uint32_t length) {
    if (mem_find_folder(folder) < 0) {
        return FS_ERR_NOT_FOUND;
    }
    if (mem_find_file(folder, name) >= 0) {
        return FS_ERR_EXISTS;
    }

    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].f_used) {
            continue;
        }
        // the first free slot. Copy the contents before touching it, so that running out of
        // memory leaves nothing half made
        char* copy = malloc(length + 1);
        if (copy == NULL) {
            return FS_ERR_NO_MEMORY;
        }
        memcpy(copy, contents, length);
        copy[length] = '\0'; // so text files can be used as C strings

        struct file* file = &files[i];
        memset(file, 0, sizeof(*file));
        file->f_used = true;
        file->f_id = next_file_id;
        next_file_id++;
        rtc_datetime(file->f_datetime, sizeof(file->f_datetime)); // "DD-MM-YY--HH:MM:SS"
        strlcpy(file->f_name, name, sizeof(file->f_name));
        strlcpy(file->f_path, folder, sizeof(file->f_path));
        file->f_cont = copy;
        file->f_size = (int)length;
        f_c++;
        return FS_OK;
    }
    return FS_ERR_FULL;
}

// replace a file's text (append = false), or add to the end of it (append = true).
// (f_datetime is when the file was made, so it doesn't change here)
static int mem_write_file (const char* folder, const char* name, const char* text, bool append) {
    int index = mem_find_file(folder, name);
    if (index < 0) {
        return FS_ERR_NOT_FOUND;
    }
    struct file* file = &files[index];

    // appending keeps the old text and writes after it, replacing writes from the start
    size_t keep_length = 0;
    if (append) {
        keep_length = strlen(file->f_cont);
    }
    size_t add_length = strlen(text);

    // grow (or shrink) the buffer. If realloc fails, the old buffer is still there, untouched
    char* contents = realloc(file->f_cont, keep_length + add_length + 1);
    if (contents == NULL) {
        return FS_ERR_NO_MEMORY;
    }
    memcpy(contents + keep_length, text, add_length + 1); // + 1 copies the '\0' as well
    file->f_cont = contents;
    file->f_size = (int)(keep_length + add_length);
    return FS_OK;
}

// replace a file's contents with `length` bytes of data (which may contain '\0').
// wipe_old fills the old buffer with zeros before freeing it, so the old contents can't turn
// up again in some later malloc
static int mem_replace_file (const char* folder, const char* name, const void* data, uint32_t length, bool wipe_old) {
    int index = mem_find_file(folder, name);
    if (index < 0) {
        return FS_ERR_NOT_FOUND;
    }
    char* contents = malloc(length + 1);
    if (contents == NULL) {
        return FS_ERR_NO_MEMORY;
    }
    memcpy(contents, data, length);
    contents[length] = '\0';

    struct file* file = &files[index];
    if (wipe_old) {
        memset(file->f_cont, 0, (size_t)file->f_size);
    }
    free(file->f_cont);
    file->f_cont = contents;
    file->f_size = (int)length;
    return FS_OK;
}

// remove a file from the table and free its contents
static int mem_delete_file (const char* folder, const char* name) {
    int index = mem_find_file(folder, name);
    if (index < 0) {
        return FS_ERR_NOT_FOUND;
    }
    free(files[index].f_cont);
    memset(&files[index], 0, sizeof(files[index])); // this clears f_used too, so the slot is free
    f_c--;
    return FS_OK;
}

// remove a folder from the table, but only if nothing is directly inside it.
// (Checking only what is DIRECTLY inside is enough: anything deeper is inside one of those)
static int mem_delete_folder (const char* path) {
    int index = mem_find_folder(path);
    if (index < 0) {
        return FS_ERR_NOT_FOUND;
    }
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (directories[i].d_used && is_directly_inside(path, directories[i].d_path)) {
            return FS_ERR_NOT_EMPTY; // a folder inside it
        }
    }
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].f_used && StrCompare(files[i].f_path, path)) {
            return FS_ERR_NOT_EMPTY; // a file inside it
        }
    }
    memset(&directories[index], 0, sizeof(directories[index]));
    d_c--;
    return FS_OK;
}

// call fn for everything directly inside a folder: folders first, then files.
// Returns how many things there were, or -1 if there is no such folder
static int mem_list (const char* path, fs_list_fn fn, void* context) {
    if (mem_find_folder(path) < 0) {
        return -1;
    }
    int count = 0;
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (directories[i].d_used && is_directly_inside(path, directories[i].d_path)) {
            char name[FS_NAME_MAX + 1];
            folder_name(directories[i].d_path, path, name, sizeof(name));
            fn(name, true, 0, context);
            count++;
        }
    }
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].f_used && StrCompare(files[i].f_path, path)) {
            fn(files[i].f_name, false, (uint32_t)files[i].f_size, context);
            count++;
        }
    }
    return count;
}

// print what is in a folder, for LA: folders first, then files.
// Returns how many lines were printed
static int mem_print (const char* path) {
    int shown = 0;
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (directories[i].d_used && is_directly_inside(path, directories[i].d_path)) {
            // the name is what comes after the parent's path, '/' and all: "cache/" from "/sys/cache/"
            char name[FS_NAME_MAX + 1];
            strlcpy(name, directories[i].d_path + strlen(path), sizeof(name));
            prtf("  <DIR>   %-22s %s\n", name, directories[i].d_name);
            shown++;
        }
    }
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].f_used && StrCompare(files[i].f_path, path)) {
            prtf("  <FILE>  %-22s %6d bytes  %s\n", files[i].f_name, files[i].f_size, files[i].f_datetime);
            shown++;
        }
    }
    return shown;
}


// =================================================================================================
//  The public functions
// =================================================================================================
//
// Each one cleans up the path it was given (relative paths, ".", ".."), then hands the clean
// absolute path to the disk driver (DISK mode) or the memory tables above (VFS mode).

bool fs_dir_exists (const char* d_path) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(d_path, path, sizeof(path))) {
        return false; // too long to be a real folder
    }
    if (on_disk()) {
        return fat_dir_exists(path);
    }
    return mem_find_folder(path) >= 0;
}

bool fs_file_exists (const char* f_path, const char* f_name) {
    char folder[FS_PATH_MAX];
    if (!fs_resolve_dir(f_path, folder, sizeof(folder))) {
        return false;
    }
    if (on_disk()) {
        return fat_file_exists(folder, f_name);
    }
    return mem_find_file(folder, f_name) >= 0;
}

// make a folder. d_name is a description LA shows next to it (VFS mode only: a FAT disk has
// nowhere to keep one). The top folder always exists, so "/" gives FS_ERR_EXISTS
int fs_create_d (const char* d_path, const char* d_name) {
    char path[FS_PATH_MAX];
    if (d_path == NULL || !fs_resolve_dir(d_path, path, sizeof(path))) {
        return FS_ERR_BAD_NAME;
    }
    if (StrCompare(path, "/")) {
        return FS_ERR_EXISTS;
    }
    if (on_disk()) {
        return fat_create_dir(path);
    }
    return mem_create_folder(path, d_name);
}

// make a text file. f_cont may be NULL, for an empty file
int fs_create_f (const char* f_path, const char* f_name, const char* f_cont) {
    if (f_cont == NULL) {
        f_cont = "";
    }
    return fs_create_bytes(f_path, f_name, f_cont, (uint32_t)strlen(f_cont));
}

// make a file from `length` bytes of data (which may contain '\0', e.g. a picture)
int fs_create_bytes (const char* f_path, const char* f_name, const void* data, uint32_t length) {
    char folder[FS_PATH_MAX];
    if (f_path == NULL || !fs_resolve_dir(f_path, folder, sizeof(folder))) {
        return FS_ERR_NOT_FOUND;
    }
    if (!fs_valid_name(f_name)) {
        return FS_ERR_BAD_NAME;
    }
    if (on_disk()) {
        return fat_create_file(folder, f_name, data, length);
    }
    return mem_create_file(folder, f_name, data, length);
}

// replace (append = false) or add to (append = true) the text of a file that is already there
int fs_write_f (const char* f_path, const char* f_name, const char* text, bool append) {
    char folder[FS_PATH_MAX];
    if (text == NULL || !fs_resolve_dir(f_path, folder, sizeof(folder))) {
        return FS_ERR_NOT_FOUND;
    }
    if (on_disk()) {
        return fat_write_file(folder, f_name, text, append);
    }
    return mem_write_file(folder, f_name, text, append);
}

// replace the contents of a file that is already there with `length` bytes of data
int fs_write_bytes (const char* f_path, const char* f_name, const void* data, uint32_t length, bool wipe_old) {
    char folder[FS_PATH_MAX];
    if (data == NULL || !fs_resolve_dir(f_path, folder, sizeof(folder))) {
        return FS_ERR_NOT_FOUND;
    }
    if (on_disk()) {
        return fat_replace_file(folder, f_name, data, length, wipe_old);
    }
    return mem_replace_file(folder, f_name, data, length, wipe_old);
}

// a copy of a text file's contents, or NULL if there is no such file. The copy is the
// caller's to change, and to free() when done with it
char* fs_read_f (const char* f_path, const char* f_name) {
    char folder[FS_PATH_MAX];
    if (!fs_resolve_dir(f_path, folder, sizeof(folder))) {
        return NULL;
    }
    if (on_disk()) {
        return fat_read_file(folder, f_name, NULL);
    }
    int index = mem_find_file(folder, f_name);
    if (index < 0) {
        return NULL;
    }
    return strdup(files[index].f_cont);
}

// like fs_read_f, but for any file: also says how many bytes it is (*size)
char* fs_read_bytes (const char* f_path, const char* f_name, uint32_t* size) {
    char folder[FS_PATH_MAX];
    if (!fs_resolve_dir(f_path, folder, sizeof(folder))) {
        return NULL;
    }
    if (on_disk()) {
        return fat_read_file(folder, f_name, size);
    }

    int index = mem_find_file(folder, f_name);
    if (index < 0) {
        return NULL;
    }
    size_t length = (size_t)files[index].f_size;
    char* copy = malloc(length + 1);
    if (copy != NULL) {
        memcpy(copy, files[index].f_cont, length + 1); // the '\0' after it too
        *size = (uint32_t)length;
    }
    return copy;
}

// call fn for every file and folder in a folder. Returns how many there were, or -1 if
// the folder isn't there
int fs_list (const char* d_path, fs_list_fn fn, void* ctx) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(d_path, path, sizeof(path))) {
        return -1;
    }
    if (on_disk()) {
        return fat_list_dir(path, fn, ctx);
    }
    return mem_list(path, fn, ctx);
}

int fs_delete_f (const char* f_path, const char* f_name) {
    char folder[FS_PATH_MAX];
    if (!fs_resolve_dir(f_path, folder, sizeof(folder))) {
        return FS_ERR_NOT_FOUND;
    }
    if (on_disk()) {
        return fat_delete_file(folder, f_name);
    }
    return mem_delete_file(folder, f_name);
}

// delete an empty folder. Not "/", and not the current folder or one it is inside
int fs_delete_d (const char* d_path) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(d_path, path, sizeof(path)) || StrCompare(path, "/")) {
        return FS_ERR_NOT_FOUND;
    }
    // If the current folder starts with this path, it IS this folder or is somewhere inside it.
    // The '/' on the end of every path stops look-alikes matching: "/system/" doesn't start
    // with "/sys/"
    if (strncmp(cd, path, strlen(path)) == 0) {
        return FS_ERR_BUSY;
    }
    if (on_disk()) {
        return fat_delete_dir(path);
    }
    return mem_delete_folder(path);
}


// =================================================================================================
//  Counting and emptying whole folders (DEL on a folder, and the three-strikes wipe)
// =================================================================================================

// ---- fs_count_files -----------------------------------------------------------------------------

// what count_entry() is adding up, handed to it through fs_list's `context`
typedef struct {
    const char* folder; // the folder being listed
    int files;          // files found so far
} file_count_t;

// fs_list() calls this for each thing in the folder: a file counts one, a folder counts
// everything inside it (by counting that folder the same way)
static void count_entry (const char* name, bool is_dir, uint32_t size, void* context) {
    (void)size;
    file_count_t* count = context;
    busy(); // counting a big folder takes a moment: let a spinner turn

    if (!is_dir) {
        count->files++;
        return;
    }
    char inner_folder[FS_PATH_MAX];
    snprintf(inner_folder, sizeof(inner_folder), "%s%s/", count->folder, name);
    int inside = fs_count_files(inner_folder);
    if (inside > 0) {
        count->files += inside;
    }
}

int fs_count_files (const char* d_path) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(d_path, path, sizeof(path)) || !fs_dir_exists(path)) {
        return FS_ERR_NOT_FOUND;
    }
    file_count_t count = { path, 0 };
    fs_list(path, count_entry, &count);
    return count.files;
}

// ---- fs_empty_dir -------------------------------------------------------------------------------
//
// Deleting things from a folder while fs_list() is still walking through that same folder
// would confuse the walk. So emptying a folder goes in rounds:
//
//     1. list up to BATCH_SIZE names (just collect them, don't touch anything)
//     2. delete those (emptying any folder among them first, the same way)
//     3. repeat until the folder is empty
//
// It also stops if a round couldn't delete anything at all, rather than go round forever.

#define BATCH_SIZE 32

// the names collected in one round, handed to collect_entry() through fs_list's `context`
typedef struct {
    char (*names)[FS_NAME_MAX];  // BATCH_SIZE names
    bool* is_folder;             // BATCH_SIZE flags: is names[i] a folder?
    int count;
} batch_t;

static void collect_entry (const char* name, bool is_dir, uint32_t size, void* context) {
    (void)size;
    batch_t* batch = context;
    if (batch->count == BATCH_SIZE) {
        return; // full: the rest wait for the next round
    }
    strlcpy(batch->names[batch->count], name, FS_NAME_MAX);
    batch->is_folder[batch->count] = is_dir;
    batch->count++;
}

int fs_empty_dir (const char* d_path, bool zero_first, fs_deleted_fn on_file, void* ctx) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(d_path, path, sizeof(path)) || !fs_dir_exists(path)) {
        return FS_ERR_NOT_FOUND;
    }

    // 32 names of 32 bytes is 1KB, too much to put on the kernel's small stack
    char (*names)[FS_NAME_MAX] = malloc(BATCH_SIZE * FS_NAME_MAX);
    if (names == NULL) {
        return FS_ERR_NO_MEMORY;
    }
    bool is_folder[BATCH_SIZE];
    int files_deleted = 0;

    for (;;) {
        batch_t batch = { names, is_folder, 0 };
        fs_list(path, collect_entry, &batch);

        int deleted_this_round = 0;
        for (int i = 0; i < batch.count; i++) {
            if (is_folder[i]) {
                // a folder has to be empty before it can be deleted: empty it first
                char inner_folder[FS_PATH_MAX];
                snprintf(inner_folder, sizeof(inner_folder), "%s%s/", path, names[i]);
                int inside = fs_empty_dir(inner_folder, zero_first, on_file, ctx);
                if (inside > 0) {
                    files_deleted += inside;
                }
                if (fs_delete_d(inner_folder) == FS_OK) {
                    deleted_this_round++;
                }
            } else {
                // Zeroing first matters for a secure wipe: deleting a file only marks its space
                // as free, and the old bytes would still be there for anyone reading the disk
                if (zero_first) {
                    bool wipe_old = true;
                    fs_write_bytes(path, names[i], "", 0, wipe_old);
                }
                if (fs_delete_f(path, names[i]) == FS_OK) {
                    files_deleted++;
                    deleted_this_round++;
                    if (on_file != NULL) {
                        on_file(names[i], ctx);
                    }
                }
            }
        }

        bool folder_is_empty = batch.count == 0;
        bool stuck = deleted_this_round == 0;
        if (folder_is_empty || stuck) {
            break;
        }
    }
    free(names);
    return files_deleted;
}


// =================================================================================================
//  The current folder, LA, and starting up
// =================================================================================================

// change the current folder. `input` can be anything fs_resolve_dir() understands
// ("..", "/sys", "cache/")
int fs_set_cd (const char* input) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(input, path, sizeof(path)) || !fs_dir_exists(path)) {
        return FS_ERR_NOT_FOUND;
    }
    strlcpy(cd, path, sizeof(cd));
    return FS_OK;
}

// LA: list what is in the current folder
void fs_print (void) {
    if (on_disk()) {
        prtf("Directory of %s  (disk)\n", cd);
    } else {
        prtf("Directory of %s\n", cd);
    }

    // both return how many lines they printed (the disk one -1 if it couldn't read the folder)
    int shown;
    if (on_disk()) {
        shown = fat_print_dir(cd);
    } else {
        shown = mem_print(cd);
    }

    if (shown == 0) {
        prtf("  (empty)\n");
    } else if (shown < 0) {
        prtf("  (could not read the directory)\n");
    }
}

// make a folder unless it is already there. Returns 1 if it was made, 0 if not (so the
// results can simply be added up)
static int make_folder_if_missing (const char* path, const char* description) {
    if (fs_dir_exists(path)) {
        return 0;
    }
    return fs_create_d(path, description) == FS_OK ? 1 : 0;
}

// the same for a file
static int make_file_if_missing (const char* folder, const char* name, const char* contents) {
    if (fs_file_exists(folder, name)) {
        return 0;
    }
    return fs_create_f(folder, name, contents) == FS_OK ? 1 : 0;
}

// Make the folders and files AerOS needs. On a disk only the missing ones are made, so the
// first boot sets a new disk up. Every name fits the 8.3 format, or the disk driver couldn't
// make it, and a parent comes before what goes in it ("/sys/" before "/sys/cache/")
static void make_base_folders (void) {
    if (on_disk()) {
        prtf("< CHK REQ FS .. >\n");
    } else {
        prtf("< MK TMP REQ FS .. >\n");
        // a FAT disk always has a top folder, the VFS has to make its own (fs_create_d refuses "/")
        mem_create_folder("/", "root");
    }

    int made = 0;
    made += make_folder_if_missing("/acc/", "accounts");
    made += make_folder_if_missing("/bin/", "bin");
    made += make_folder_if_missing("/docs/", "the manual");
    made += make_folder_if_missing("/sys/", "system");
    made += make_folder_if_missing("/sys/cache/", "system cache");
    made += make_folder_if_missing("/sys/tmp/", "system temp");
    made += make_file_if_missing("/", "conf.aer", "# here you will find all major system config options");

    if (on_disk()) {
        prtf("< REQ FS DONE .. > %d missing item(s) created\n", made);
    } else {
        prtf("< TMP REQ FS DONE .. > %d directories, %d files\n", d_c, f_c);
    }
    prtf("[E:fs0f1]\n"); // the boot log's marker for the end of the file system stage
}

// boot.c calls this once, with:
//     "n"  a normal boot: use an AEROS disk if there is one, otherwise the temporary file system
//     "d"  a demo boot: the temporary file system, without even looking at the disks
//     "p"  just print the current folder
// Always returns 0
int fs_init (const char* arg) {
    bool demo = StrCompare(arg, "d");
    bool normal = StrCompare(arg, "n");

    if (StrCompare(arg, "p")) {
        fs_print();
        return 0;
    }
    if (!normal && !demo) {
        return 0;
    }

    // In demo mode the disks aren't touched at all, so fat_mount_auto() is never called
    bool found_disk = !demo && fat_mount_auto(fs_disk_status, sizeof(fs_disk_status));

    if (found_disk) {
        fs_mode = FS_MODE_DISK;
        prtf("[ OK ] Disk: %s\n", fs_disk_status);
        prtf("< INIT DISK FS .. >\n");
    } else {
        fs_mode = FS_MODE_MEMORY;
        if (demo) {
            prtf("[INFO] Demo mode: disks are not loaded, files will not be saved\n");
        } else {
            // fs_disk_status says why, e.g. "no AEROS disk found"
            prtf("[INFO] Disk: %s, files will not be saved\n", fs_disk_status);
        }
        prtf("< INIT TMP FS .. >\n");
    }
    make_base_folders();
    return 0;
}

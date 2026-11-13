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

    One folder is neither on the disk nor in the tables: /src/, AerOS's own source code, which
    is built into the kernel (see sys/source.h). Every public function checks for it first and
    hands it to the source_ functions, so you can LA, RD and SD around in it like anywhere else,
    in both modes. Anything that would change it gets FS_ERR_READ_ONLY.

    And files in home folders remember their past: before one is changed or deleted, the
    functions here hand it to versions_keep() (sys/versions.h), which keeps a copy, and a name
    like "notes.txt@2" reads one of those copies instead of the file.

    https://wiki.osdev.org/VFS
*/
#include "f_sys.h"
#include "source.h"
#include "versions.h"
#include "sys_clck.h"
#include "../lib/primary_definitions.h"
#include "../lib/busy.h"
#include "trace.h"

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
    if (name == NULL || name[0] == '\0' || slen(name) >= FS_NAME_MAX) {
        return false;
    }
    // "." and ".." mean "here" and "the folder above" in a path, so they can't be names
    if (seq(name, ".") || seq(name, "..")) {
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
    size_t length = slen(path);
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
        scpy(path, "/", sizeof(path));
    } else {
        scpy(path, cd, sizeof(path));
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
        size_t path_length = slen(path);
        bool name_too_long = part_length >= FS_NAME_MAX;
        bool path_too_long = path_length + part_length + 2 > sizeof(path);
        if (name_too_long || path_too_long) {
            return false;
        }
        memcpy(path + path_length, part, part_length);
        path[path_length + part_length] = '/';
        path[path_length + part_length + 1] = '\0';
    }

    // scpy returns how long the string is; if that doesn't fit, `out` was too small
    return scpy(out, path, out_size) < out_size;
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
        scpy(dir_out, cd, dir_size);
        scpy(name_out, input, name_size);
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
        scpy(name_out, last_slash + 1, name_size);
    }
    return fs_valid_name(name_out);
}

// the folder above a folder: "/sys/cache/" -> "/sys/" ("/" is its own parent).
// d_path must already be a clean absolute path ending in '/'
void fs_parent_path (const char* d_path, char* out, size_t out_size) {
    scpy(out, d_path, out_size);
    go_up_one_folder(out);
}

// is `path` a folder directly inside `parent`? With parent "/sys/":
//     "/sys/cache/"       yes
//     "/sys/cache/old/"   no, that's inside a folder inside it
//     "/sys/"             no, that's the parent itself
static bool is_directly_inside (const char* parent, const char* path) {
    size_t parent_length = slen(parent);
    size_t path_length = slen(path);

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
        if (directories[i].d_used && seq(directories[i].d_path, path)) {
            return i;
        }
    }
    return -1;
}

// where the file with this name in this folder is in files[], or -1 if there is none.
// (In the VFS names care about upper and lower case, unlike on a FAT disk)
static int mem_find_file (const char* folder, const char* name) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].f_used && seq(files[i].f_path, folder) && seq(files[i].f_name, name)) {
            return i;
        }
    }
    return -1;
}

// the name of a folder from its path, without the '/' on the end, written into `name`.
// `parent` is the path of the folder it is in: "/sys/cache/" in "/sys/" gives "cache"
static void folder_name (const char* path, const char* parent, char* name, size_t size) {
    scpy(name, path + slen(parent), size);
    size_t length = slen(name);
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
    // folders are only a flat list of paths, so this check is what keeps them a proper tree
    char parent[FS_PATH_MAX];
    fs_parent_path(path, parent, sizeof(parent));
    bool is_top = seq(path, "/");
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
        scpy(folder->d_path, path, sizeof(folder->d_path));
        scpy(folder->d_name, description != NULL ? description : "", sizeof(folder->d_name));
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
        scpy(file->f_name, name, sizeof(file->f_name));
        scpy(file->f_path, folder, sizeof(file->f_path));
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
        keep_length = slen(file->f_cont);
    }
    size_t add_length = slen(text);

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

    // the new copy is ready, so the old buffer can go (the slot is never left without contents)
    struct file* file = &files[index];
    if (wipe_old) {
        memset(file->f_cont, 0, (size_t)file->f_size);
    }
    free(file->f_cont);
    file->f_cont = contents;
    file->f_size = (int)length;
    return FS_OK;
}

// write `length` bytes at `offset` in a file in memory, in place: the rest stays as it is.
// Writing past the end makes the buffer bigger, with zeros in any gap. data NULL writes zeros.
// The buffer always keeps a '\0' after the contents, so it can still be read as text
static int mem_write_at (const char* folder, const char* name, uint32_t offset, const void* data, uint32_t length) {
    int index = mem_find_file(folder, name);
    if (index < 0) {
        return FS_ERR_NOT_FOUND;
    }
    // f_size is an int, so a file in memory stops just short of 2 GB (and the end of the write
    // must fit: offset > MAX - length, written so the sum itself can't overflow)
    if (length > 0x7FFFFFFFu || offset > 0x7FFFFFFFu - length) {
        return FS_ERR_FULL;
    }
    struct file* file = &files[index];
    uint32_t old_size = (uint32_t)file->f_size;
    uint32_t end = offset + length;

    // longer: grow the buffer (+ 1 for the '\0'), with the new part zeroed. If realloc fails the
    // old buffer is still there, untouched
    if (end > old_size) {
        char* contents = realloc(file->f_cont, end + 1);
        if (contents == NULL) {
            return FS_ERR_NO_MEMORY;
        }
        memset(contents + old_size, 0, end - old_size + 1);
        file->f_cont = contents;
        file->f_size = (int)end;
    }
    if (data != NULL) {
        memcpy(file->f_cont + offset, data, length);
    } else {
        memset(file->f_cont + offset, 0, length);
    }
    return FS_OK;
}

// make a file in memory `size` bytes long: shorter cuts off its end, longer adds zeros
static int mem_truncate (const char* folder, const char* name, uint32_t size) {
    int index = mem_find_file(folder, name);
    if (index < 0) {
        return FS_ERR_NOT_FOUND;
    }
    struct file* file = &files[index];
    uint32_t old_size = (uint32_t)file->f_size;
    if (size >= old_size) {
        return mem_write_at(folder, name, old_size, NULL, size - old_size);
    }
    // shorter: zero what is cut off (so it can't turn up again), and end the text there. The
    // buffer stays the size it was: it is freed with the file
    memset(file->f_cont + size, 0, old_size - size);
    file->f_size = (int)size;
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
        if (files[i].f_used && seq(files[i].f_path, path)) {
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
        if (files[i].f_used && seq(files[i].f_path, path)) {
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
            scpy(name, directories[i].d_path + slen(path), sizeof(name));
            prtf("  <DIR>   %-22s %s\n", name, directories[i].d_name);
            shown++;
        }
    }
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].f_used && seq(files[i].f_path, path)) {
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
// A path inside /src/ goes to the built-in source files instead (sys/source.c).

// a copy of a built-in source file, with a '\0' after it so it can be used as text too.
// free() it when done. *size (if not NULL) gets its length
static char* copy_source_file (const source_file_t* file, uint32_t* size) {
    uint32_t length = source_size(file);
    char* copy = malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, file->start, length);
    copy[length] = '\0';
    if (size != NULL) {
        *size = length;
    }
    return copy;
}

// is there a folder at this path? (relative paths, "." and ".." are fine)
bool fs_dir_exists (const char* d_path) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(d_path, path, sizeof(path))) {
        return false; // too long to be a real folder
    }
    if (source_in(path)) {
        return source_folder_exists(path);
    }
    if (on_disk()) {
        return fat_dir_exists(path);
    }
    return mem_find_folder(path) >= 0;
}

// is there a file with this name in this folder? An old version like "notes.txt@2" counts too
bool fs_file_exists (const char* f_path, const char* f_name) {
    char folder[FS_PATH_MAX];
    if (!fs_resolve_dir(f_path, folder, sizeof(folder))) {
        return false;
    }
    // the built-in source files keep no old versions, so the answer is simply yes or no
    if (source_in(folder)) {
        return source_find(folder, f_name) != NULL;
    }
    // first the real file, on the disk or in the tables
    bool exists;
    if (on_disk()) {
        exists = fat_file_exists(folder, f_name);
    } else {
        exists = mem_find_file(folder, f_name) >= 0;
    }
    // "notes.txt@2", an old version of notes.txt (see sys/versions.h), counts as a file too
    char base[FS_NAME_MAX];
    int number;
    if (!exists && versions_split_name(f_name, base, sizeof(base), &number)) {
        // versions are numbered 1 (the newest) upwards, so @number exists if at least
        // that many are kept
        version_t list[VERSIONS_KEPT];
        exists = versions_list(folder, base, list) >= number;
    }
    return exists;
}

// make a folder. d_name is a description LA shows next to it (VFS mode only: a FAT disk has
// nowhere to keep one). The top folder always exists, so "/" gives FS_ERR_EXISTS
int fs_create_d (const char* d_path, const char* d_name) {
    char path[FS_PATH_MAX];
    if (d_path == NULL || !fs_resolve_dir(d_path, path, sizeof(path))) {
        return FS_ERR_BAD_NAME;
    }
    if (seq(path, "/")) {
        return FS_ERR_EXISTS;
    }
    if (source_in(path)) {
        TRACE("%s is inside /src/, which can't be changed", path);
        return FS_ERR_READ_ONLY; // also "/src/" itself, which would hide the built-in one
    }
    TRACE("making the folder %s %s", path, on_disk() ? "on the disk" : "in memory (no disk)");
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
    return fs_create_bytes(f_path, f_name, f_cont, (uint32_t)slen(f_cont));
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
    if (source_in(folder)) {
        TRACE("%s%s is inside /src/, which can't be changed", folder, f_name);
        return FS_ERR_READ_ONLY;
    }
    TRACE("making %s%s, %u bytes, %s", folder, f_name, (unsigned int)length,
          on_disk() ? "on the disk" : "in memory (no disk)");
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
    if (source_in(folder)) {
        return FS_ERR_READ_ONLY;
    }
    versions_keep(folder, f_name); // the file as it is now, before it changes
    TRACE("%s %s%s", append ? "adding to" : "replacing the text of", folder, f_name);
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
    if (source_in(folder)) {
        return FS_ERR_READ_ONLY;
    }
    // Keep the file as it is now, before it changes. Unless the old bytes are being wiped
    // (encryption): then any copies of them are wiped too
    if (wipe_old) {
        versions_forget(folder, f_name);
    } else {
        versions_keep(folder, f_name);
    }
    TRACE("replacing %s%s with %u bytes%s", folder, f_name, (unsigned int)length,
          wipe_old ? ", wiping the old bytes as it goes" : "");
    if (on_disk()) {
        return fat_replace_file(folder, f_name, data, length, wipe_old);
    }
    return mem_replace_file(folder, f_name, data, length, wipe_old);
}

// write `length` bytes at `offset` in a file that is already there, in place (see f_sys.h).
// Unlike the other writes it keeps no old version: a database changes a page at a time, often,
// and copying the whole file each time would be far too slow. Call versions_keep() once first
// if a version is wanted
int fs_write_at (const char* f_path, const char* f_name, uint32_t offset, const void* data, uint32_t length) {
    char folder[FS_PATH_MAX];
    if (!fs_resolve_dir(f_path, folder, sizeof(folder))) {
        return FS_ERR_NOT_FOUND;
    }
    if (source_in(folder)) {
        TRACE("%s%s is inside /src/, which can't be changed", folder, f_name);
        return FS_ERR_READ_ONLY;
    }
    if (on_disk()) {
        return fat_write_at(folder, f_name, offset, data, length);
    }
    TRACE("writing %u bytes at byte %u of %s%s, in memory", (unsigned int)length,
          (unsigned int)offset, folder, f_name);
    return mem_write_at(folder, f_name, offset, data, length);
}

// make a file `size` bytes long, in place (see f_sys.h). Keeps no old version, like fs_write_at
int fs_truncate (const char* f_path, const char* f_name, uint32_t size) {
    char folder[FS_PATH_MAX];
    if (!fs_resolve_dir(f_path, folder, sizeof(folder))) {
        return FS_ERR_NOT_FOUND;
    }
    if (source_in(folder)) {
        TRACE("%s%s is inside /src/, which can't be changed", folder, f_name);
        return FS_ERR_READ_ONLY;
    }
    if (on_disk()) {
        return fat_truncate(folder, f_name, size);
    }
    TRACE("making %s%s %u bytes long, in memory", folder, f_name, (unsigned int)size);
    return mem_truncate(folder, f_name, size);
}

// a copy of a text file's contents, or NULL if there is no such file. The copy is the
// caller's to change, and to free() when done with it
char* fs_read_f (const char* f_path, const char* f_name) {
    uint32_t size; // (a '\0' always follows what fs_read_bytes reads, so it is a string too)
    return fs_read_bytes(f_path, f_name, &size);
}

// fs_read_bytes for a folder that has been cleaned up already
static char* read_file (const char* folder, const char* name, uint32_t* size) {
    if (source_in(folder)) {
        const source_file_t* file = source_find(folder, name);
        return file != NULL ? copy_source_file(file, size) : NULL;
    }
    if (on_disk()) {
        return fat_read_file(folder, name, size);
    }

    // VFS: hand back a copy, so the caller can't change or free the table's own buffer
    int index = mem_find_file(folder, name);
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

// like fs_read_f, but for any file: also says how many bytes it is (*size)
char* fs_read_bytes (const char* f_path, const char* f_name, uint32_t* size) {
    char folder[FS_PATH_MAX];
    if (!fs_resolve_dir(f_path, folder, sizeof(folder))) {
        return NULL;
    }
    char* contents = read_file(folder, f_name, size);

    // no such file: is it "notes.txt@2", an old version of notes.txt? (see sys/versions.h)
    char base[FS_NAME_MAX];
    int number;
    if (contents == NULL && versions_split_name(f_name, base, sizeof(base), &number)) {
        TRACE("no file called %s: reading version %d of %s instead", f_name, number, base);
        contents = versions_read(folder, base, number, size);
    }
    if (contents != NULL) {
        TRACE("read %s%s, %u bytes, from %s", folder, f_name, (unsigned int)*size,
              source_in(folder) ? "AerOS's own source" : on_disk() ? "the disk" : "memory");
    } else {
        TRACE("%s%s isn't there", folder, f_name);
    }
    return contents;
}

// open a file to read a piece at a time with fs_read_at(), false if it isn't there.
// fs_file_t remembers which of the three places the file lives in, so each read goes straight there
bool fs_open (const char* path, fs_file_t* file) {
    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (!fs_split_path(path, folder, sizeof(folder), name, sizeof(name))) {
        return false;
    }
    // built_in stays NULL unless it is a source file: fs_read_at() checks it first
    file->built_in = NULL;
    // a built-in source file: just point at its bytes in the kernel image
    if (source_in(folder)) {
        const source_file_t* source = source_find(folder, name);
        if (source == NULL) {
            return false;
        }
        file->built_in = source->start;
        file->size = source_size(source);
        return true;
    }
    // on the disk: the FAT driver keeps its own place in the cluster chain inside file->fat
    file->on_disk = on_disk();
    if (file->on_disk) {
        if (!fat_open(folder, name, &file->fat)) {
            return false;
        }
        file->size = file->fat.size;
        return true;
    }
    // in the VFS: remember its slot in files[] (fs_read_at() checks the slot is still in use)
    file->vfs_slot = mem_find_file(folder, name);
    if (file->vfs_slot < 0) {
        return false;
    }
    file->size = (uint32_t)files[file->vfs_slot].f_size;
    return true;
}

// copy up to `length` bytes from `offset` out of a file that is all in memory already
static int32_t copy_part (const char* bytes, uint32_t size, uint32_t offset, void* out, uint32_t length) {
    if (offset >= size) {
        return 0;
    }
    if (length > size - offset) {
        length = size - offset;
    }
    memcpy(out, bytes + offset, length);
    return (int32_t)length;
}

// read up to length bytes from offset of a file fs_open() opened: how many were read, 0 at
// the end, -1 on an error
int32_t fs_read_at (fs_file_t* file, uint32_t offset, void* out, uint32_t length) {
    if (file->built_in != NULL) {
        return copy_part(file->built_in, file->size, offset, out, length);
    }
    if (file->on_disk) {
        return fat_read_at(&file->fat, offset, out, length);
    }
    // in memory the whole file is already there: just copy the part that's wanted
    const struct file* entry = &files[file->vfs_slot];
    if (!entry->f_used) {
        return -1; // deleted since it was opened
    }
    return copy_part(entry->f_cont, (uint32_t)entry->f_size, offset, out, length);
}

// call fn for every file and folder in a folder. Returns how many there were, or -1 if
// the folder isn't there
int fs_list (const char* d_path, fs_list_fn fn, void* ctx) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(d_path, path, sizeof(path))) {
        return -1;
    }
    // (listing "/" doesn't include src: then DEL / and the wipe would try to delete it, and
    // every count of "/" would include about 100 source files)
    if (source_in(path)) {
        return source_list(path, fn, ctx);
    }
    if (on_disk()) {
        return fat_list_dir(path, fn, ctx);
    }
    return mem_list(path, fn, ctx);
}

// delete a file, keeping a copy of it first so it can be brought back
int fs_delete_f (const char* f_path, const char* f_name) {
    char folder[FS_PATH_MAX];
    if (!fs_resolve_dir(f_path, folder, sizeof(folder))) {
        return FS_ERR_NOT_FOUND;
    }
    if (source_in(folder)) {
        TRACE("%s%s is inside /src/, which can't be changed", folder, f_name);
        return FS_ERR_READ_ONLY;
    }
    versions_keep(folder, f_name); // so UNDO can bring it back
    TRACE("deleting %s%s %s", folder, f_name, on_disk() ? "from the disk" : "from memory");
    if (on_disk()) {
        return fat_delete_file(folder, f_name);
    }
    return mem_delete_file(folder, f_name);
}

// delete an empty folder. Not "/", and not the current folder or one it is inside
int fs_delete_d (const char* d_path) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(d_path, path, sizeof(path)) || seq(path, "/")) {
        return FS_ERR_NOT_FOUND;
    }
    if (source_in(path)) {
        return FS_ERR_READ_ONLY;
    }
    // If the current folder starts with this path, it IS this folder or is somewhere inside it.
    // The '/' on the end of every path stops look-alikes matching: "/system/" doesn't start
    // with "/sys/"
    if (strncmp(cd, path, slen(path)) == 0) {
        TRACE("not deleting %s: you are in it", path);
        return FS_ERR_BUSY;
    }
    TRACE("deleting the empty folder %s", path);
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
    sfmt(inner_folder, sizeof(inner_folder), "%s%s/", count->folder, name);
    int inside = fs_count_files(inner_folder);
    if (inside > 0) {
        count->files += inside;
    }
}

// how many files are in a folder, counting everything in the folders inside it too
// (folders themselves don't count). FS_ERR_NOT_FOUND if the folder isn't there
int fs_count_files (const char* d_path) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(d_path, path, sizeof(path)) || !fs_dir_exists(path)) {
        return FS_ERR_NOT_FOUND;
    }
    // count_entry() calls back into fs_count_files() for each folder inside, so this walks
    // the whole tree below `path`
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

// fs_list() calls this for each thing in the folder: note down its name and whether it is a
// folder, until the batch is full
static void collect_entry (const char* name, bool is_dir, uint32_t size, void* context) {
    (void)size;
    batch_t* batch = context;
    if (batch->count == BATCH_SIZE) {
        return; // full: the rest wait for the next round
    }
    scpy(batch->names[batch->count], name, FS_NAME_MAX);
    batch->is_folder[batch->count] = is_dir;
    batch->count++;
}

// delete everything inside a folder (but not the folder itself), in the rounds described above.
// zero_first overwrites each file before deleting it, on_file (may be NULL) is told about every
// file that goes. Returns how many files were deleted, or an FS_ERR_ code
int fs_empty_dir (const char* d_path, bool zero_first, fs_deleted_fn on_file, void* ctx) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(d_path, path, sizeof(path)) || !fs_dir_exists(path)) {
        return FS_ERR_NOT_FOUND;
    }
    if (source_in(path)) {
        return FS_ERR_READ_ONLY;
    }

    // 32 names of 32 bytes is 1KB, too much to put on the kernel's small stack
    char (*names)[FS_NAME_MAX] = malloc(BATCH_SIZE * FS_NAME_MAX);
    if (names == NULL) {
        return FS_ERR_NO_MEMORY;
    }
    bool is_folder[BATCH_SIZE];
    int files_deleted = 0;

    // A whole folder going doesn't keep versions of its files (sys/versions.h): that would
    // copy everything in it just to delete it. (And in the wipe, the versions go too.)
    versions_pause();
    for (;;) {
        // step 1: collect up to BATCH_SIZE names (the same two buffers are reused every round)
        batch_t batch = { names, is_folder, 0 };
        fs_list(path, collect_entry, &batch);

        // step 2: delete them, now that the listing has finished
        int deleted_this_round = 0;
        for (int i = 0; i < batch.count; i++) {
            if (is_folder[i]) {
                // a folder has to be empty before it can be deleted: empty it first
                char inner_folder[FS_PATH_MAX];
                sfmt(inner_folder, sizeof(inner_folder), "%s%s/", path, names[i]);
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

        // step 3: go round again, unless there was nothing left or nothing could be deleted
        // (e.g. the current folder is in here, fs_delete_d() refuses it with FS_ERR_BUSY)
        bool folder_is_empty = batch.count == 0;
        bool stuck = deleted_this_round == 0;
        if (folder_is_empty || stuck) {
            break;
        }
    }
    versions_resume();
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
    scpy(cd, path, sizeof(cd));
    return FS_OK;
}

// LA: list what is in the current folder
void fs_print (void) {
    if (source_in(cd)) {
        prtf("Directory of %s  (AerOS's own source code, read only)\n", cd);
        source_print(cd);
        return;
    }
    if (on_disk()) {
        prtf("Directory of %s  (disk)\n", cd);
    } else {
        prtf("Directory of %s\n", cd);
    }

    // all three return how many lines they printed (the disk one -1 if it couldn't read it)
    int shown = 0;
    if (seq(cd, "/")) {
        // /src/ isn't on the disk or in the tables, so it gets its own line
        prtf("  <DIR>   %-22s %s\n", "src/", "AerOS's own source code (read only)");
        shown++;
    }
    int listed;
    if (on_disk()) {
        listed = fat_print_dir(cd);
    } else {
        listed = mem_print(cd);
    }
    // a read error (-1) wins over the src/ line, so the message below says what went wrong
    shown = listed < 0 ? listed : shown + listed;

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
    made += make_folder_if_missing("/bin/games/", "games");
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
    bool demo = seq(arg, "d");
    bool normal = seq(arg, "n");

    if (seq(arg, "p")) {
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

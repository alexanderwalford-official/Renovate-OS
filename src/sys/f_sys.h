/*
    f_sys.h
    The file system the rest of the OS uses. It runs in one of two modes:
    - DISK mode: a FAT disk labelled RENOVATE was found, every file is saved to it (sys/fat.c).
    - VFS mode: no disk, files live in a temporary (in memory) file system and are lost on restart.
    (There is also DEMO mode, picked from the boot menu: it is just VFS mode where we never even
    look at the disks, see fs_init("d"). Handy when booting on someone else's computer.)

    Directory paths are always absolute and end in a slash, e.g. "/", "/sys/", "/sys/cache/".
    A file lives in a directory (f_path) and has a name (f_name).
    Functions return FS_OK (0) or a negative FS_ERR_ code, fs_strerror() describes it.

    Why the trailing slash? It makes a lot of string handling trivial: a sub directory's path is
    just its parent's path + "name/", and "is X inside Y?" becomes "does X start with Y?" without
    "/sys/" accidentally matching "/system/".

    The public fs_ functions accept paths as the user typed them (relative to the current
    directory cd, with "." and ".." allowed), they clean them up with fs_resolve_dir() first and
    then hand the absolute path to the FAT driver or the in-memory tables.
*/
#ifndef F_SYS_H
#define F_SYS_H

#include <stddef.h>
#include <stdbool.h>
#include "fat.h"

// size limits of the temporary file system (fixed size tables, so no malloc for the bookkeeping)
#define FS_MAX_FILES    64  // how many files the VFS can hold
#define FS_MAX_DIRS     32  // how many directories the VFS can hold (the root "/" counts as one)
#define FS_PATH_MAX     96  // longest directory path, including the trailing '/' and the '\0'
#define FS_NAME_MAX     32  // longest file / directory name + 1 for the '\0' (so 31 characters)
#define FS_DATETIME_MAX 20  // "DD-MM-YY--HH:MM:SS" is 18 characters + '\0', with one spare

// the values fs_mode can have
#define FS_MODE_MEMORY 0  // VFS mode: temporary file system in RAM (also used for demo mode)
#define FS_MODE_DISK   1  // DISK mode: everything goes to the mounted FAT volume

// a file in the temporary file system (VFS mode only, the disk keeps its own directory entries)
typedef struct file {
    int f_id; // ID
    int f_size; // size in bytes
    char f_path[FS_PATH_MAX]; // path / folder
    char f_name[FS_NAME_MAX]; // name
    char f_datetime[FS_DATETIME_MAX]; // date & time created
    char* f_cont; // contents as string (allocated with malloc)
    bool f_used; // slot in use
} file_t;

// a directory in the temporary file system (VFS mode only)
// directories are not stored as a tree: each one just knows its full path, and the parent /
// child relationship is worked out by comparing paths (see fs_is_child() in f_sys.c)
typedef struct directory {
    int d_id;                 // ID, counts up from 1 and is never reused
    char d_path[FS_PATH_MAX]; // full path ending in '/', e.g. "/sys/cache/"
    char d_name[FS_NAME_MAX]; // a description shown next to it by LA, e.g. "system cache"
    bool d_used;              // slot in use
} directory_t;

extern int fs_mode; // FS_MODE_MEMORY or FS_MODE_DISK, set once by fs_init()
extern char fs_disk_status[]; // what happened when looking for a disk (shown in the boot log)
extern int f_c; // file count (VFS mode)
extern int d_c; // directory count (VFS mode)
extern char cd[FS_PATH_MAX]; // current directory

// path helpers, user input may be relative to cd and use "." and ".."

// is this a usable file / directory name? (not empty, not too long, no '/', not "." or "..", printable)
bool fs_valid_name (const char* name);
// turn user input into a clean absolute directory path ending in '/', false if it would be too long
bool fs_resolve_dir (const char* input, char* out, size_t out_size);
// split a file path ("../notes.txt") into its absolute directory and the file name, false if invalid
bool fs_split_path (const char* input, char* dir_out, size_t dir_size, char* name_out, size_t name_size);
// the parent of an absolute directory path, "/sys/cache/" -> "/sys/" ("/" stays "/")
void fs_parent_path (const char* d_path, char* out, size_t out_size);

// does this directory exist?
bool fs_dir_exists (const char* d_path);
// does this file exist in directory f_path?
bool fs_file_exists (const char* f_path, const char* f_name);
// create a directory, d_name is a description (VFS mode only, the disk has nowhere to keep it)
int fs_create_d (const char* d_path, const char* d_name);
// create a file holding f_cont (NULL = empty), fails with FS_ERR_EXISTS if it is already there
int fs_create_f (const char* f_path, const char* f_name, const char* f_cont);
// replace (append = false) or add to (append = true) the contents of an existing file
int fs_write_f (const char* f_path, const char* f_name, const char* text, bool append);
char* fs_read_f (const char* f_path, const char* f_name); // free() the result
// delete a file
int fs_delete_f (const char* f_path, const char* f_name);
// delete an empty directory (not "/", the current directory or one of its parents)
int fs_delete_d (const char* d_path);
// change the current directory (cd), FS_ERR_NOT_FOUND if it does not exist
int fs_set_cd (const char* input);
// list the current directory on screen
void fs_print (void);
// "n" = normal boot (find a disk or fall back to the VFS), "d" = demo boot (VFS only), "p" = print cd
int fs_init (const char* arg);

#endif /* F_SYS_H */

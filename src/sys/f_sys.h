/*
    f_sys.h
    The file system the rest of the OS uses. It runs in one of two modes:
    - DISK mode: a FAT disk labelled RENOVATE was found, every file is saved to it (sys/fat.c).
    - VFS mode: no disk, files live in a temporary (in memory) file system and are lost on restart.

    Directory paths are always absolute and end in a slash, e.g. "/", "/sys/", "/sys/cache/".
    A file lives in a directory (f_path) and has a name (f_name).
    Functions return FS_OK (0) or a negative FS_ERR_ code, fs_strerror() describes it.
*/
#ifndef F_SYS_H
#define F_SYS_H

#include <stddef.h>
#include <stdbool.h>
#include "fat.h"

#define FS_MAX_FILES    64
#define FS_MAX_DIRS     32
#define FS_PATH_MAX     96
#define FS_NAME_MAX     32
#define FS_DATETIME_MAX 20

#define FS_MODE_MEMORY 0
#define FS_MODE_DISK   1

typedef struct file {
    int f_id; // ID
    int f_size; // size in bytes
    char f_path[FS_PATH_MAX]; // path / folder
    char f_name[FS_NAME_MAX]; // name
    char f_datetime[FS_DATETIME_MAX]; // date & time created
    char* f_cont; // contents as string (allocated with malloc)
    bool f_used; // slot in use
} file_t;

typedef struct directory {
    int d_id;
    char d_path[FS_PATH_MAX];
    char d_name[FS_NAME_MAX];
    bool d_used;
} directory_t;

extern int fs_mode;
extern char fs_disk_status[]; // what happened when looking for a disk (shown in the boot log)
extern int f_c; // file count (VFS mode)
extern int d_c; // directory count (VFS mode)
extern char cd[FS_PATH_MAX]; // current directory

// path helpers, user input may be relative to cd and use "." and ".."
bool fs_valid_name (const char* name);
bool fs_resolve_dir (const char* input, char* out, size_t out_size);
bool fs_split_path (const char* input, char* dir_out, size_t dir_size, char* name_out, size_t name_size);
void fs_parent_path (const char* d_path, char* out, size_t out_size);

bool fs_dir_exists (const char* d_path);
bool fs_file_exists (const char* f_path, const char* f_name);
int fs_create_d (const char* d_path, const char* d_name);
int fs_create_f (const char* f_path, const char* f_name, const char* f_cont);
int fs_write_f (const char* f_path, const char* f_name, const char* text, bool append);
char* fs_read_f (const char* f_path, const char* f_name); // free() the result
int fs_delete_f (const char* f_path, const char* f_name);
int fs_delete_d (const char* d_path);
int fs_set_cd (const char* input);
void fs_print (void);
int fs_init (const char* arg);

#endif /* F_SYS_H */

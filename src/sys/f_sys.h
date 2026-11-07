#ifndef F_SYS_H
#define F_SYS_H

/*
    f_sys.h
    Temporary (in memory) virtual file system - VFS.
    Nothing is written to a disk, everything is lost on restart.

    Directory paths are always absolute and end in a slash, e.g. "/", "/sys/", "/sys/cache/".
    A file lives in a directory (f_path) and has a name (f_name).
*/

#include <stdint.h>
#include <stdbool.h>
#include "../drivers/video/VGA_linear.h"
#include "../lib/primary_definitions.h"
#include "sys_clck.h"

#define FS_MAX_FILES    64
#define FS_MAX_DIRS     32
#define FS_PATH_MAX     96
#define FS_NAME_MAX     32
#define FS_DATETIME_MAX 20

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

// dynamic vars
int f_c = 0; // file count
int d_c = 0; // directory count
int f_next_id = 1;
int d_next_id = 1;
struct file files[FS_MAX_FILES];
struct directory directories[FS_MAX_DIRS];
char cd[FS_PATH_MAX] = "/"; // current directory

/* ---------------- lookups & path helpers ---------------- */

// index of the directory with this exact path, or -1
int fs_find_d(const char* d_path) {
    for (int i = 0; i < FS_MAX_DIRS; ++i) {
        if (directories[i].d_used && StrCompare(directories[i].d_path, d_path)) {
            return i;
        }
    }
    return -1;
}

// index of the file with this name in this directory, or -1
int fs_find_f(const char* f_path, const char* f_name) {
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].f_used && StrCompare(files[i].f_path, f_path) && StrCompare(files[i].f_name, f_name)) {
            return i;
        }
    }
    return -1;
}

// a valid file / directory name: not empty, no slashes, not "." or ".."
bool fs_valid_name(const char* name) {
    if (name == NULL || name[0] == '\0' || strlen(name) >= FS_NAME_MAX) {
        return false;
    }
    if (StrCompare(name, ".") || StrCompare(name, "..")) {
        return false;
    }
    for (const char* p = name; *p; ++p) {
        if (*p == '/' || !isprint((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

// turn a path typed by the user (absolute or relative to cd, may use "." and "..") into
// a clean absolute directory path ending in '/'. Returns false if it is too long.
bool fs_resolve_dir(const char* input, char* out, size_t out_size) {
    char work[FS_PATH_MAX];

    if (input == NULL || input[0] == '\0') {
        input = ".";
    }
    strlcpy(work, (input[0] == '/') ? "/" : cd, sizeof(work));

    const char* p = input;
    while (*p) {
        while (*p == '/') p++;
        if (*p == '\0') break;

        const char* start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);

        if (len == 1 && start[0] == '.') {
            continue; // "." = stay here
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            // ".." = go up one level (stays at "/" when already there)
            size_t wl = strlen(work);
            if (wl > 1) {
                work[wl - 1] = '\0';
                char* slash = strrchr(work, '/');
                slash[1] = '\0';
            }
            continue;
        }
        if (len >= FS_NAME_MAX) {
            return false;
        }
        size_t wl = strlen(work);
        if (wl + len + 2 > sizeof(work)) {
            return false;
        }
        memcpy(work + wl, start, len);
        work[wl + len] = '/';
        work[wl + len + 1] = '\0';
    }

    if (strlen(work) + 1 > out_size) {
        return false;
    }
    strlcpy(out, work, out_size);
    return true;
}

// split a file path typed by the user ("notes.txt", "/sys/conf.ren", "../a.txt") into its
// directory path and file name
bool fs_split_path(const char* input, char* dir_out, size_t dir_size, char* name_out, size_t name_size) {
    if (input == NULL || input[0] == '\0') {
        return false;
    }
    const char* slash = strrchr(input, '/');
    if (slash == NULL) {
        strlcpy(dir_out, cd, dir_size);
        strlcpy(name_out, input, name_size);
    } else {
        char dir_part[FS_PATH_MAX];
        size_t len = (size_t)(slash - input) + 1;
        if (len >= sizeof(dir_part)) {
            return false;
        }
        memcpy(dir_part, input, len);
        dir_part[len] = '\0';
        if (!fs_resolve_dir(dir_part, dir_out, dir_size)) {
            return false;
        }
        strlcpy(name_out, slash + 1, name_size);
    }
    return fs_valid_name(name_out);
}

// the parent of a directory path, e.g. "/sys/cache/" -> "/sys/"
void fs_parent_path(const char* d_path, char* out, size_t out_size) {
    strlcpy(out, d_path, out_size);
    size_t len = strlen(out);
    if (len <= 1) {
        return; // "/" has no parent
    }
    out[len - 1] = '\0';
    char* slash = strrchr(out, '/');
    slash[1] = '\0';
}

// true if d_path is a direct sub directory of parent
bool fs_is_child(const char* parent, const char* d_path) {
    size_t plen = strlen(parent);
    size_t dlen = strlen(d_path);
    if (dlen <= plen || strncmp(d_path, parent, plen) != 0) {
        return false;
    }
    // the rest must be "name/" with exactly one slash at the end
    const char* rest = d_path + plen;
    return strchr(rest, '/') == d_path + dlen - 1;
}

/* ---------------- create / write / delete ---------------- */

// create a directory, returns its ID or -1
int fs_create_d (const char* d_path, const char* d_name) {
    char path[FS_PATH_MAX];
    char parent[FS_PATH_MAX];

    if (d_path == NULL || !fs_resolve_dir(d_path, path, sizeof(path))) {
        return -1;
    }
    if (fs_find_d(path) >= 0) {
        return -1; // already exists
    }
    fs_parent_path(path, parent, sizeof(parent));
    if (!StrCompare(path, "/") && fs_find_d(parent) < 0) {
        return -1; // parent directory missing
    }

    for (int i = 0; i < FS_MAX_DIRS; ++i) {
        if (!directories[i].d_used) {
            struct directory* d = &directories[i];
            memset(d, 0, sizeof(*d));
            d->d_used = true;
            d->d_id = d_next_id++;
            strlcpy(d->d_path, path, sizeof(d->d_path));
            strlcpy(d->d_name, d_name ? d_name : "", sizeof(d->d_name));
            d_c = d_c + 1;
            return d->d_id;
        }
    }
    return -1; // no space left
}

// create a file, returns its ID or -1
int fs_create_f (const char* f_path, const char* f_name, const char* f_cont) {
    char path[FS_PATH_MAX];

    if (f_path == NULL || !fs_resolve_dir(f_path, path, sizeof(path))) {
        return -1;
    }
    if (!fs_valid_name(f_name) || fs_find_d(path) < 0 || fs_find_f(path, f_name) >= 0) {
        return -1; // bad name, missing directory or the file already exists
    }

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (!files[i].f_used) {
            struct file* f = &files[i];
            char* contents = strdup(f_cont ? f_cont : "");
            if (contents == NULL) {
                return -1; // out of memory
            }
            memset(f, 0, sizeof(*f));
            f->f_used = true;
            f->f_id = f_next_id++;
            rtc_datetime(f->f_datetime, sizeof(f->f_datetime));
            strlcpy(f->f_name, f_name, sizeof(f->f_name));
            strlcpy(f->f_path, path, sizeof(f->f_path));
            f->f_cont = contents;
            f->f_size = (int)strlen(contents);
            f_c = f_c + 1;
            return f->f_id;
        }
    }
    return -1; // no space left
}

// replace (append = false) or add to (append = true) the contents of a file
int fs_write_f (const char* f_path, const char* f_name, const char* text, bool append) {
    int index = fs_find_f(f_path, f_name);
    if (index < 0 || text == NULL) {
        return -1;
    }
    struct file* f = &files[index];
    size_t old_len = append ? strlen(f->f_cont) : 0;
    size_t add_len = strlen(text);
    char* contents = (char*)realloc(f->f_cont, old_len + add_len + 1);
    if (contents == NULL) {
        return -1;
    }
    memcpy(contents + old_len, text, add_len + 1);
    f->f_cont = contents;
    f->f_size = (int)(old_len + add_len);
    return f->f_size;
}

// contents of a file, or NULL if it does not exist
const char* fs_read_f (const char* f_path, const char* f_name) {
    int index = fs_find_f(f_path, f_name);
    return (index < 0) ? NULL : files[index].f_cont;
}

int fs_delete_f (const char* f_path, const char* f_name) {
    int index = fs_find_f(f_path, f_name);
    if (index < 0) {
        return -1;
    }
    free(files[index].f_cont);
    memset(&files[index], 0, sizeof(files[index]));
    f_c = f_c - 1;
    return 0;
}

// delete an empty directory (not "/" and not the current directory or one of its parents)
int fs_delete_d (const char* d_path) {
    int index = fs_find_d(d_path);
    if (index < 0 || StrCompare(d_path, "/") || strncmp(cd, d_path, strlen(d_path)) == 0) {
        return -1;
    }
    for (int i = 0; i < FS_MAX_DIRS; ++i) {
        if (directories[i].d_used && fs_is_child(d_path, directories[i].d_path)) {
            return -1; // not empty
        }
    }
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].f_used && StrCompare(files[i].f_path, d_path)) {
            return -1; // not empty
        }
    }
    memset(&directories[index], 0, sizeof(directories[index]));
    d_c = d_c - 1;
    return 0;
}

// change the current directory, returns 0 on success
int fs_set_cd (const char* input) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(input, path, sizeof(path)) || fs_find_d(path) < 0) {
        return -1;
    }
    strlcpy(cd, path, sizeof(cd));
    return 0;
}

/* ---------------- init & listing ---------------- */

void fs_print () {
    // print all files & subdirectories in cd
    int shown = 0;
    printf("Directory of %s\n", cd); // print current dir
    for (int i = 0; i < FS_MAX_DIRS; ++i) {
        if (directories[i].d_used && fs_is_child(cd, directories[i].d_path)) {
            // found a directory directly inside cd
            char name[FS_NAME_MAX + 1];
            strlcpy(name, directories[i].d_path + strlen(cd), sizeof(name));
            printf("  <DIR>   %-22s %s\n", name, directories[i].d_name);
            shown++;
        }
    }
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].f_used && StrCompare(files[i].f_path, cd)) {
            printf("  <FILE>  %-22s %6d bytes  %s\n", files[i].f_name, files[i].f_size, files[i].f_datetime);
            shown++;
        }
    }
    if (shown == 0) {
        printf("  (empty)\n");
    }
}

// init all base os files
void fs_init_base () {
    printf("< MK TMP REQ FS .. >\n");
    // ..
    fs_create_d("/", "root");
    fs_create_d("/acc/", "accounts");
    fs_create_d("/bin/", "bin");
    fs_create_d("/sys/", "system");
    fs_create_d("/sys/cache/", "system cache");
    fs_create_d("/sys/tmp/", "system temp");
    fs_create_f("/", "conf.ren", "# here you will find all major system config options");

    printf("< TMP REQ FS DONE .. > %d directories, %d files\n", d_c, f_c);
    printf("[E:fs0f1]\n");
}

// fs init: "n" = create the base file system, "p" = print the current directory
int fs_init (const char* arg) {
    printf("< INIT TMP FS .. >\n");
    if (StrCompare(arg, "n")) {
        fs_init_base();
    } else if (StrCompare(arg, "p")) {
        fs_print();
    }
    return 0;
}

#endif /* f_sys_h */

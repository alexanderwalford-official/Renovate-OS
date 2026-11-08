/*
    f_sys.c
    Picks the disk (FAT) or the temporary in memory file system, see f_sys.h.
*/
#include "f_sys.h"
#include "sys_clck.h"
#include "../lib/primary_definitions.h"

// dynamic vars
int fs_mode = FS_MODE_MEMORY;
char fs_disk_status[96] = "";
int f_c = 0;
int d_c = 0;
char cd[FS_PATH_MAX] = "/";

static int f_next_id = 1;
static int d_next_id = 1;
static struct file files[FS_MAX_FILES];
static struct directory directories[FS_MAX_DIRS];

// a valid file / directory name: not empty, no slashes, not "." or ".."
bool fs_valid_name (const char* name) {
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

// "/sys/cache/" -> "/sys/" in place, "/" stays "/"
static void fs_go_up (char* path) {
    size_t len = strlen(path);
    if (len > 1) {
        path[len - 1] = '\0';
        strrchr(path, '/')[1] = '\0';
    }
}

// turn a path typed by the user (absolute or relative to cd, may use "." and "..") into
// a clean absolute directory path ending in '/'. Returns false if it is too long.
bool fs_resolve_dir (const char* input, char* out, size_t out_size) {
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
        size_t len = strcspn(p, "/");
        p += len;

        if (len == 1 && start[0] == '.') {
            continue; // "." = stay here
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            fs_go_up(work); // ".." = go up one level (stays at "/" when already there)
            continue;
        }
        size_t wl = strlen(work);
        if (len >= FS_NAME_MAX || wl + len + 2 > sizeof(work)) {
            return false;
        }
        memcpy(work + wl, start, len);
        work[wl + len] = '/';
        work[wl + len + 1] = '\0';
    }
    return strlcpy(out, work, out_size) < out_size;
}

// split a file path typed by the user ("notes.txt", "/sys/conf.ren", "../a.txt") into its
// directory path and file name
bool fs_split_path (const char* input, char* dir_out, size_t dir_size, char* name_out, size_t name_size) {
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
void fs_parent_path (const char* d_path, char* out, size_t out_size) {
    strlcpy(out, d_path, out_size);
    fs_go_up(out);
}

// true if d_path is a direct sub directory of parent
static bool fs_is_child (const char* parent, const char* d_path) {
    size_t plen = strlen(parent);
    size_t dlen = strlen(d_path);
    if (dlen <= plen || strncmp(d_path, parent, plen) != 0) {
        return false;
    }
    // the rest must be "name/" with exactly one slash at the end
    return strchr(d_path + plen, '/') == d_path + dlen - 1;
}

// VFS mode: index of the directory with this exact path, or -1
static int mem_find_d (const char* d_path) {
    for (int i = 0; i < FS_MAX_DIRS; ++i) {
        if (directories[i].d_used && StrCompare(directories[i].d_path, d_path)) {
            return i;
        }
    }
    return -1;
}

// index of the file with this name in this directory, or -1
static int mem_find_f (const char* f_path, const char* f_name) {
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].f_used && StrCompare(files[i].f_path, f_path) && StrCompare(files[i].f_name, f_name)) {
            return i;
        }
    }
    return -1;
}

static int mem_create_d (const char* path, const char* d_name) {
    char parent[FS_PATH_MAX];
    if (mem_find_d(path) >= 0) {
        return FS_ERR_EXISTS;
    }
    fs_parent_path(path, parent, sizeof(parent));
    if (!StrCompare(path, "/") && mem_find_d(parent) < 0) {
        return FS_ERR_NOT_FOUND; // parent directory missing
    }
    for (int i = 0; i < FS_MAX_DIRS; ++i) {
        if (!directories[i].d_used) {
            struct directory* d = &directories[i];
            memset(d, 0, sizeof(*d));
            d->d_used = true;
            d->d_id = d_next_id++;
            strlcpy(d->d_path, path, sizeof(d->d_path));
            strlcpy(d->d_name, d_name ? d_name : "", sizeof(d->d_name));
            d_c++;
            return FS_OK;
        }
    }
    return FS_ERR_FULL;
}

static int mem_create_f (const char* path, const char* f_name, const char* f_cont) {
    if (mem_find_d(path) < 0) return FS_ERR_NOT_FOUND;
    if (mem_find_f(path, f_name) >= 0) return FS_ERR_EXISTS;

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (!files[i].f_used) {
            struct file* f = &files[i];
            char* contents = strdup(f_cont);
            if (contents == NULL) {
                return FS_ERR_NO_MEMORY;
            }
            memset(f, 0, sizeof(*f));
            f->f_used = true;
            f->f_id = f_next_id++;
            rtc_datetime(f->f_datetime, sizeof(f->f_datetime));
            strlcpy(f->f_name, f_name, sizeof(f->f_name));
            strlcpy(f->f_path, path, sizeof(f->f_path));
            f->f_cont = contents;
            f->f_size = (int)strlen(contents);
            f_c++;
            return FS_OK;
        }
    }
    return FS_ERR_FULL;
}

static int mem_write_f (const char* path, const char* f_name, const char* text, bool append) {
    int index = mem_find_f(path, f_name);
    if (index < 0) {
        return FS_ERR_NOT_FOUND;
    }
    struct file* f = &files[index];
    size_t old_len = append ? strlen(f->f_cont) : 0;
    size_t add_len = strlen(text);
    char* contents = realloc(f->f_cont, old_len + add_len + 1);
    if (contents == NULL) {
        return FS_ERR_NO_MEMORY;
    }
    memcpy(contents + old_len, text, add_len + 1);
    f->f_cont = contents;
    f->f_size = (int)(old_len + add_len);
    return FS_OK;
}

static int mem_delete_f (const char* path, const char* f_name) {
    int index = mem_find_f(path, f_name);
    if (index < 0) {
        return FS_ERR_NOT_FOUND;
    }
    free(files[index].f_cont);
    memset(&files[index], 0, sizeof(files[index]));
    f_c--;
    return FS_OK;
}

static int mem_delete_d (const char* path) {
    int index = mem_find_d(path);
    if (index < 0) {
        return FS_ERR_NOT_FOUND;
    }
    for (int i = 0; i < FS_MAX_DIRS; ++i) {
        if (directories[i].d_used && fs_is_child(path, directories[i].d_path)) {
            return FS_ERR_NOT_EMPTY;
        }
    }
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].f_used && StrCompare(files[i].f_path, path)) {
            return FS_ERR_NOT_EMPTY;
        }
    }
    memset(&directories[index], 0, sizeof(directories[index]));
    d_c--;
    return FS_OK;
}

static int mem_print (const char* path) {
    int shown = 0;
    for (int i = 0; i < FS_MAX_DIRS; ++i) {
        if (directories[i].d_used && fs_is_child(path, directories[i].d_path)) {
            // found a directory directly inside path
            char name[FS_NAME_MAX + 1];
            strlcpy(name, directories[i].d_path + strlen(path), sizeof(name));
            printf("  <DIR>   %-22s %s\n", name, directories[i].d_name);
            shown++;
        }
    }
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].f_used && StrCompare(files[i].f_path, path)) {
            printf("  <FILE>  %-22s %6d bytes  %s\n", files[i].f_name, files[i].f_size, files[i].f_datetime);
            shown++;
        }
    }
    return shown;
}

bool fs_dir_exists (const char* d_path) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(d_path, path, sizeof(path))) return false;
    return (fs_mode == FS_MODE_DISK) ? fat_dir_exists(path) : mem_find_d(path) >= 0;
}

bool fs_file_exists (const char* f_path, const char* f_name) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(f_path, path, sizeof(path))) return false;
    return (fs_mode == FS_MODE_DISK) ? fat_file_exists(path, f_name) : mem_find_f(path, f_name) >= 0;
}

// create a directory, d_name is a description shown by LA (VFS mode only)
int fs_create_d (const char* d_path, const char* d_name) {
    char path[FS_PATH_MAX];
    if (d_path == NULL || !fs_resolve_dir(d_path, path, sizeof(path))) return FS_ERR_BAD_NAME;
    if (StrCompare(path, "/")) return FS_ERR_EXISTS;
    return (fs_mode == FS_MODE_DISK) ? fat_create_dir(path) : mem_create_d(path, d_name);
}

// create a file
int fs_create_f (const char* f_path, const char* f_name, const char* f_cont) {
    char path[FS_PATH_MAX];
    if (f_path == NULL || !fs_resolve_dir(f_path, path, sizeof(path))) return FS_ERR_NOT_FOUND;
    if (!fs_valid_name(f_name)) return FS_ERR_BAD_NAME;
    if (f_cont == NULL) f_cont = "";
    return (fs_mode == FS_MODE_DISK) ? fat_create_file(path, f_name, f_cont) : mem_create_f(path, f_name, f_cont);
}

// replace (append = false) or add to (append = true) the contents of a file
int fs_write_f (const char* f_path, const char* f_name, const char* text, bool append) {
    char path[FS_PATH_MAX];
    if (text == NULL || !fs_resolve_dir(f_path, path, sizeof(path))) return FS_ERR_NOT_FOUND;
    return (fs_mode == FS_MODE_DISK) ? fat_write_file(path, f_name, text, append) : mem_write_f(path, f_name, text, append);
}

// the contents of a file in a new buffer (free() it when done), or NULL
char* fs_read_f (const char* f_path, const char* f_name) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(f_path, path, sizeof(path))) return NULL;
    if (fs_mode == FS_MODE_DISK) {
        return fat_read_file(path, f_name);
    }
    int index = mem_find_f(path, f_name);
    return (index < 0) ? NULL : strdup(files[index].f_cont);
}

int fs_delete_f (const char* f_path, const char* f_name) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(f_path, path, sizeof(path))) return FS_ERR_NOT_FOUND;
    return (fs_mode == FS_MODE_DISK) ? fat_delete_file(path, f_name) : mem_delete_f(path, f_name);
}

// delete an empty directory (not "/" and not the current directory or one of its parents)
int fs_delete_d (const char* d_path) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(d_path, path, sizeof(path)) || StrCompare(path, "/")) return FS_ERR_NOT_FOUND;
    if (strncmp(cd, path, strlen(path)) == 0) return FS_ERR_BUSY;
    return (fs_mode == FS_MODE_DISK) ? fat_delete_dir(path) : mem_delete_d(path);
}

// change the current directory
int fs_set_cd (const char* input) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(input, path, sizeof(path)) || !fs_dir_exists(path)) {
        return FS_ERR_NOT_FOUND;
    }
    strlcpy(cd, path, sizeof(cd));
    return FS_OK;
}

void fs_print (void) {
    // print all files & subdirectories in cd
    printf("Directory of %s%s\n", cd, (fs_mode == FS_MODE_DISK) ? "  (disk)" : ""); // print current dir
    int shown = (fs_mode == FS_MODE_DISK) ? fat_print_dir(cd) : mem_print(cd);
    if (shown == 0) {
        printf("  (empty)\n");
    } else if (shown < 0) {
        printf("  (could not read the directory)\n");
    }
}

// make a directory / file unless it already exists, returns 1 if it was created
static int fs_ensure_d (const char* d_path, const char* d_name) {
    return fs_dir_exists(d_path) ? 0 : (fs_create_d(d_path, d_name) == FS_OK);
}

static int fs_ensure_f (const char* f_path, const char* f_name, const char* f_cont) {
    return fs_file_exists(f_path, f_name) ? 0 : (fs_create_f(f_path, f_name, f_cont) == FS_OK);
}

// init all base os files (on a disk only the missing ones are created, so the first boot sets it up)
static void fs_init_base (void) {
    int created = 0;
    printf((fs_mode == FS_MODE_DISK) ? "< CHK REQ FS .. >\n" : "< MK TMP REQ FS .. >\n");
    // ..
    if (fs_mode == FS_MODE_MEMORY) {
        mem_create_d("/", "root");
    }
    created += fs_ensure_d("/acc/", "accounts");
    created += fs_ensure_d("/bin/", "bin");
    created += fs_ensure_d("/sys/", "system");
    created += fs_ensure_d("/sys/cache/", "system cache");
    created += fs_ensure_d("/sys/tmp/", "system temp");
    created += fs_ensure_f("/", "conf.ren", "# here you will find all major system config options");

    if (fs_mode == FS_MODE_DISK) {
        printf("< REQ FS DONE .. > %d missing item(s) created\n", created);
    } else {
        printf("< TMP REQ FS DONE .. > %d directories, %d files\n", d_c, f_c);
    }
    printf("[E:fs0f1]\n");
}

// fs init: "n" = find a disk (or use the temporary file system) and create the base files, "p" = print the current directory
int fs_init (const char* arg) {
    bool demo = StrCompare(arg, "d");
    if (StrCompare(arg, "n") || demo) {
        // look for a RENOVATE disk and use it if there is one (demo mode never touches the disks)
        if (!demo && fat_mount_auto(fs_disk_status, sizeof(fs_disk_status))) {
            fs_mode = FS_MODE_DISK;
            printf("[ OK ] Disk: %s\n", fs_disk_status);
            printf("< INIT DISK FS .. >\n");
        } else {
            fs_mode = FS_MODE_MEMORY;
            if (demo) {
                printf("[INFO] Demo mode: disks are not loaded, files will not be saved\n");
            } else {
                printf("[INFO] Disk: %s, files will not be saved\n", fs_disk_status);
            }
            printf("< INIT TMP FS .. >\n");
        }
        fs_init_base();
    } else if (StrCompare(arg, "p")) {
        fs_print();
    }
    return 0;
}

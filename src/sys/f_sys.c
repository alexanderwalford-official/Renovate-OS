/*
    f_sys.c
    Picks the disk (FAT) or the temporary in memory file system, see f_sys.h.

    This is the layer the shell talks to. It does three jobs:

    1. Paths. Users type things like "notes.txt", "../bin", "/sys/./cache" or "sys//tmp". We turn
       all of those into one clean, absolute form ("/sys/cache/") before anything else looks at
       them, so neither the FAT driver nor the memory tables ever have to deal with "." or "..".

    2. Picking a backend. fs_init() runs once at boot and sets fs_mode:
       - "n" (normal boot): look for a FAT disk labelled AEROS. Found one? DISK mode, every
         call is passed on to sys/fat.c. Otherwise VFS mode.
       - "d" (demo boot): straight to VFS mode, the disks are never even read.
       After that every public fs_ function is a tiny wrapper: resolve the path, then
       "fs_mode == FS_MODE_DISK ? fat_something() : mem_something()".

    3. The temporary file system (VFS mode, the mem_ functions). It is deliberately simple:
       two fixed size arrays, files[] and directories[], each slot has a "used" flag, and
       everything is found by a linear search. Directories are kept as a flat list of full
       paths rather than a tree, a file just remembers the path of the directory it is in.
       Only the file contents are malloc'd. It is all gone when the computer restarts.

    https://wiki.osdev.org/VFS
*/
#include "f_sys.h"
#include "sys_clck.h"
#include "../lib/primary_definitions.h"

// dynamic vars
int fs_mode = FS_MODE_MEMORY;  // VFS until fs_init() finds a disk
char fs_disk_status[96] = "";  // filled in by fat_mount_auto(), e.g. "no AEROS disk found"
int f_c = 0;                   // how many files[] slots are in use (VFS mode)
int d_c = 0;                   // how many directories[] slots are in use (VFS mode, includes "/")
char cd[FS_PATH_MAX] = "/";    // the current directory, always absolute and ending in '/'

// the next ID to hand out, IDs only ever count up so a deleted file's ID is never reused
static int f_next_id = 1;
static int d_next_id = 1;
// the VFS tables, a slot is free when its f_used / d_used flag is false (static, so they start zeroed)
static struct file files[FS_MAX_FILES];
static struct directory directories[FS_MAX_DIRS];

// a valid file / directory name: not empty, no slashes, not "." or ".."
// (it also has to fit in FS_NAME_MAX and only use printable characters. On a disk the FAT
// driver is stricter still: the name has to fit the 8.3 format, fat_make_83() checks that)
bool fs_valid_name (const char* name) {
    if (name == NULL || name[0] == '\0' || strlen(name) >= FS_NAME_MAX) {
        return false;
    }
    // "." and ".." mean "here" and "the parent" in a path, so they can't be names of their own
    if (StrCompare(name, ".") || StrCompare(name, "..")) {
        return false;
    }
    // a '/' would split the name into two path parts, control characters would mess up the screen
    for (const char* p = name; *p; ++p) {
        if (*p == '/' || !isprint((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

// "/sys/cache/" -> "/sys/" in place, "/" stays "/"
// how: chop off the trailing '/' ("/sys/cache"), find the last '/' that is left (the one in front
// of "cache") and end the string just after it. strrchr can't return NULL here because every
// path starts with '/'
static void fs_go_up (char* path) {
    size_t len = strlen(path);
    if (len > 1) {
        path[len - 1] = '\0';
        strrchr(path, '/')[1] = '\0';
    }
}

// turn a path typed by the user (absolute or relative to cd, may use "." and "..") into
// a clean absolute directory path ending in '/'. Returns false if it is too long.
// examples with cd = "/sys/":
//   ""           -> "/sys/"          (empty means "here")
//   "cache"      -> "/sys/cache/"
//   "../bin/"    -> "/bin/"
//   "/a//b/./c"  -> "/a/b/c/"        (repeated slashes and "." are ignored)
//   "/.."        -> "/"              (you can't go above the root)
// note this only builds the string, it does not check that the directory exists
bool fs_resolve_dir (const char* input, char* out, size_t out_size) {
    char work[FS_PATH_MAX]; // the path built so far, always ends in '/'

    if (input == NULL || input[0] == '\0') {
        input = ".";
    }
    // a leading '/' means start from the root, anything else is relative to the current directory
    strlcpy(work, (input[0] == '/') ? "/" : cd, sizeof(work));

    // walk through the input one path part ("component") at a time
    const char* p = input;
    while (*p) {
        // skip the slash(es) in front of the next part, stop if there is nothing after them
        while (*p == '/') p++;
        if (*p == '\0') break;

        // the part runs from start up to the next '/' (or the end of the string)
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
        // an ordinary name: append "name/". It needs len bytes + 1 for the '/' + 1 for the '\0'
        size_t wl = strlen(work);
        if (len >= FS_NAME_MAX || wl + len + 2 > sizeof(work)) {
            return false;
        }
        memcpy(work + wl, start, len);
        work[wl + len] = '/';
        work[wl + len + 1] = '\0';
    }
    // strlcpy returns the length it wanted to copy, if that doesn't fit out was too small
    return strlcpy(out, work, out_size) < out_size;
}

// split a file path typed by the user ("notes.txt", "/sys/conf.ren", "../a.txt") into its
// directory path and file name
// everything up to and including the last '/' is the directory (resolved like any other
// directory path), everything after it is the name. No '/' at all means the file is in cd.
// returns false if the path is empty, too long or the name is not valid (e.g. "sys/" has no name)
bool fs_split_path (const char* input, char* dir_out, size_t dir_size, char* name_out, size_t name_size) {
    if (input == NULL || input[0] == '\0') {
        return false;
    }
    const char* slash = strrchr(input, '/');
    if (slash == NULL) {
        // just a name, e.g. "notes.txt"
        strlcpy(dir_out, cd, dir_size);
        strlcpy(name_out, input, name_size);
    } else {
        // copy "../sys/" out of "../sys/a.txt" (len includes the slash) and resolve it
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
// (d_path must already be absolute and end in '/', and "/" is its own parent)
void fs_parent_path (const char* d_path, char* out, size_t out_size) {
    strlcpy(out, d_path, out_size);
    fs_go_up(out);
}

// true if d_path is a direct sub directory of parent
// e.g. parent "/sys/": "/sys/cache/" yes, "/sys/cache/old/" no (a grandchild), "/sys/" no (itself)
static bool fs_is_child (const char* parent, const char* d_path) {
    size_t plen = strlen(parent);
    size_t dlen = strlen(d_path);
    // it has to be longer than the parent and start with the parent's path
    if (dlen <= plen || strncmp(d_path, parent, plen) != 0) {
        return false;
    }
    // the rest must be "name/" with exactly one slash at the end
    // (so the first '/' after the parent's part has to be the very last character)
    return strchr(d_path + plen, '/') == d_path + dlen - 1;
}

// ---- the temporary (VFS) file system --------------------------------------------------------
// all of these take clean absolute paths, the public fs_ functions below resolve them first

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
// (names are case sensitive in the VFS, unlike on a FAT disk)
static int mem_find_f (const char* f_path, const char* f_name) {
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].f_used && StrCompare(files[i].f_path, f_path) && StrCompare(files[i].f_name, f_name)) {
            return i;
        }
    }
    return -1;
}

// add a directory to the table, its parent has to exist already (except for "/" itself)
// returns FS_OK, FS_ERR_EXISTS, FS_ERR_NOT_FOUND (no parent) or FS_ERR_FULL (all FS_MAX_DIRS slots taken)
static int mem_create_d (const char* path, const char* d_name) {
    char parent[FS_PATH_MAX];
    if (mem_find_d(path) >= 0) {
        return FS_ERR_EXISTS;
    }
    fs_parent_path(path, parent, sizeof(parent));
    if (!StrCompare(path, "/") && mem_find_d(parent) < 0) {
        return FS_ERR_NOT_FOUND; // parent directory missing
    }
    // take the first free slot
    for (int i = 0; i < FS_MAX_DIRS; ++i) {
        if (!directories[i].d_used) {
            struct directory* d = &directories[i];
            memset(d, 0, sizeof(*d)); // clear anything a deleted directory left behind
            d->d_used = true;
            d->d_id = d_next_id++;
            strlcpy(d->d_path, path, sizeof(d->d_path));
            strlcpy(d->d_name, d_name ? d_name : "", sizeof(d->d_name)); // the description is optional
            d_c++;
            return FS_OK;
        }
    }
    return FS_ERR_FULL;
}

// add a file to the table with a copy of f_cont as its contents
// returns FS_OK, FS_ERR_NOT_FOUND (no such directory), FS_ERR_EXISTS, FS_ERR_NO_MEMORY or FS_ERR_FULL
static int mem_create_f (const char* path, const char* f_name, const char* f_cont) {
    if (mem_find_d(path) < 0) return FS_ERR_NOT_FOUND;
    if (mem_find_f(path, f_name) >= 0) return FS_ERR_EXISTS;

    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (!files[i].f_used) {
            struct file* f = &files[i];
            // copy the contents before touching the slot, so running out of memory leaves nothing half made
            char* contents = strdup(f_cont);
            if (contents == NULL) {
                return FS_ERR_NO_MEMORY;
            }
            memset(f, 0, sizeof(*f));
            f->f_used = true;
            f->f_id = f_next_id++;
            rtc_datetime(f->f_datetime, sizeof(f->f_datetime)); // "DD-MM-YY--HH:MM:SS" from the CMOS clock
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

// replace or add to a file's contents by resizing its buffer with realloc
// gotcha: f_datetime is the creation time, so it is not changed here
static int mem_write_f (const char* path, const char* f_name, const char* text, bool append) {
    int index = mem_find_f(path, f_name);
    if (index < 0) {
        return FS_ERR_NOT_FOUND;
    }
    struct file* f = &files[index];
    // appending keeps the old text and adds after it, replacing starts writing at offset 0
    size_t old_len = append ? strlen(f->f_cont) : 0;
    size_t add_len = strlen(text);
    // +1 for the '\0'. If realloc fails the old buffer is still there and untouched
    char* contents = realloc(f->f_cont, old_len + add_len + 1);
    if (contents == NULL) {
        return FS_ERR_NO_MEMORY;
    }
    memcpy(contents + old_len, text, add_len + 1); // add_len + 1 copies the '\0' too
    f->f_cont = contents;
    f->f_size = (int)(old_len + add_len);
    return FS_OK;
}

// remove a file from the table and free its contents
static int mem_delete_f (const char* path, const char* f_name) {
    int index = mem_find_f(path, f_name);
    if (index < 0) {
        return FS_ERR_NOT_FOUND;
    }
    free(files[index].f_cont);
    memset(&files[index], 0, sizeof(files[index])); // clears f_used too, so the slot is free again
    f_c--;
    return FS_OK;
}

// remove a directory from the table, only if nothing is directly inside it
// (checking direct children is enough: a grandchild can't exist without its parent, and the
// parent can't be deleted while the grandchild is in it)
static int mem_delete_d (const char* path) {
    int index = mem_find_d(path);
    if (index < 0) {
        return FS_ERR_NOT_FOUND;
    }
    // any sub directories?
    for (int i = 0; i < FS_MAX_DIRS; ++i) {
        if (directories[i].d_used && fs_is_child(path, directories[i].d_path)) {
            return FS_ERR_NOT_EMPTY;
        }
    }
    // any files?
    for (int i = 0; i < FS_MAX_FILES; ++i) {
        if (files[i].f_used && StrCompare(files[i].f_path, path)) {
            return FS_ERR_NOT_EMPTY;
        }
    }
    memset(&directories[index], 0, sizeof(directories[index]));
    d_c--;
    return FS_OK;
}

// list one VFS directory (sub directories first, then files), returns how many lines were printed
static int mem_print (const char* path) {
    int shown = 0;
    for (int i = 0; i < FS_MAX_DIRS; ++i) {
        if (directories[i].d_used && fs_is_child(path, directories[i].d_path)) {
            // found a directory directly inside path
            // its name is what comes after the parent's path, e.g. "cache/" from "/sys/cache/"
            // (up to 31 characters + the '/' + the '\0', hence FS_NAME_MAX + 1)
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

// ---- the public functions -------------------------------------------------------------------
// each one resolves the path the user gave (relative paths, ".", ".."), then hands the clean
// absolute path to the FAT driver (DISK mode) or the mem_ functions above (VFS mode)

// does this directory exist? (false too if the path is too long to resolve)
bool fs_dir_exists (const char* d_path) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(d_path, path, sizeof(path))) return false;
    return (fs_mode == FS_MODE_DISK) ? fat_dir_exists(path) : mem_find_d(path) >= 0;
}

// does a file called f_name exist in directory f_path?
bool fs_file_exists (const char* f_path, const char* f_name) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(f_path, path, sizeof(path))) return false;
    return (fs_mode == FS_MODE_DISK) ? fat_file_exists(path, f_name) : mem_find_f(path, f_name) >= 0;
}

// create a directory, d_name is a description shown by LA (VFS mode only)
// the root always exists, so asking for "/" gives FS_ERR_EXISTS (fs_init_base() makes the VFS root itself)
int fs_create_d (const char* d_path, const char* d_name) {
    char path[FS_PATH_MAX];
    if (d_path == NULL || !fs_resolve_dir(d_path, path, sizeof(path))) return FS_ERR_BAD_NAME;
    if (StrCompare(path, "/")) return FS_ERR_EXISTS;
    return (fs_mode == FS_MODE_DISK) ? fat_create_dir(path) : mem_create_d(path, d_name);
}

// create a file
// f_cont = NULL is allowed and gives an empty file
int fs_create_f (const char* f_path, const char* f_name, const char* f_cont) {
    char path[FS_PATH_MAX];
    if (f_path == NULL || !fs_resolve_dir(f_path, path, sizeof(path))) return FS_ERR_NOT_FOUND;
    if (!fs_valid_name(f_name)) return FS_ERR_BAD_NAME;
    if (f_cont == NULL) f_cont = "";
    return (fs_mode == FS_MODE_DISK) ? fat_create_file(path, f_name, f_cont) : mem_create_f(path, f_name, f_cont);
}

// replace (append = false) or add to (append = true) the contents of a file
// the file has to exist already, use fs_create_f() for a new one
int fs_write_f (const char* f_path, const char* f_name, const char* text, bool append) {
    char path[FS_PATH_MAX];
    if (text == NULL || !fs_resolve_dir(f_path, path, sizeof(path))) return FS_ERR_NOT_FOUND;
    return (fs_mode == FS_MODE_DISK) ? fat_write_file(path, f_name, text, append) : mem_write_f(path, f_name, text, append);
}

// the contents of a file in a new buffer (free() it when done), or NULL
// both modes hand back a fresh copy, so the caller can change or free it without breaking the file
char* fs_read_f (const char* f_path, const char* f_name) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(f_path, path, sizeof(path))) return NULL;
    if (fs_mode == FS_MODE_DISK) {
        return fat_read_file(path, f_name);
    }
    int index = mem_find_f(path, f_name);
    return (index < 0) ? NULL : strdup(files[index].f_cont);
}

// delete a file
int fs_delete_f (const char* f_path, const char* f_name) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(f_path, path, sizeof(path))) return FS_ERR_NOT_FOUND;
    return (fs_mode == FS_MODE_DISK) ? fat_delete_file(path, f_name) : mem_delete_f(path, f_name);
}

// delete an empty directory (not "/" and not the current directory or one of its parents)
int fs_delete_d (const char* d_path) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(d_path, path, sizeof(path)) || StrCompare(path, "/")) return FS_ERR_NOT_FOUND;
    // cd starting with path means cd is path itself or somewhere inside it. Thanks to the trailing
    // '/' this can't be fooled by similar names: cd "/system/" does not start with "/sys/"
    if (strncmp(cd, path, strlen(path)) == 0) return FS_ERR_BUSY;
    return (fs_mode == FS_MODE_DISK) ? fat_delete_dir(path) : mem_delete_d(path);
}

// change the current directory
// input can be anything fs_resolve_dir() understands ("..", "/sys", "cache/")
int fs_set_cd (const char* input) {
    char path[FS_PATH_MAX];
    if (!fs_resolve_dir(input, path, sizeof(path)) || !fs_dir_exists(path)) {
        return FS_ERR_NOT_FOUND;
    }
    strlcpy(cd, path, sizeof(cd));
    return FS_OK;
}

// list the current directory (the LA command)
void fs_print (void) {
    // print all files & subdirectories in cd
    printf("Directory of %s%s\n", cd, (fs_mode == FS_MODE_DISK) ? "  (disk)" : ""); // print current dir
    // both listers return how many entries they printed, the FAT one returns -1 if it can't find cd
    int shown = (fs_mode == FS_MODE_DISK) ? fat_print_dir(cd) : mem_print(cd);
    if (shown == 0) {
        printf("  (empty)\n");
    } else if (shown < 0) {
        printf("  (could not read the directory)\n");
    }
}

// make a directory / file unless it already exists, returns 1 if it was created
// (0 if it was already there or could not be made, so the results can simply be added up)
static int fs_ensure_d (const char* d_path, const char* d_name) {
    return fs_dir_exists(d_path) ? 0 : (fs_create_d(d_path, d_name) == FS_OK);
}

static int fs_ensure_f (const char* f_path, const char* f_name, const char* f_cont) {
    return fs_file_exists(f_path, f_name) ? 0 : (fs_create_f(f_path, f_name, f_cont) == FS_OK);
}

// init all base os files (on a disk only the missing ones are created, so the first boot sets it up)
// every name here fits the 8.3 format, otherwise the FAT driver could not create them on a disk.
// parents come before their children ("/sys/" before "/sys/cache/") because a parent has to exist first
static void fs_init_base (void) {
    int created = 0;
    printf((fs_mode == FS_MODE_DISK) ? "< CHK REQ FS .. >\n" : "< MK TMP REQ FS .. >\n");
    // ..
    // a FAT disk always has a root directory, the VFS has to make its own (fs_create_d() refuses "/")
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
    // boot log marker for the end of the file system stage (boot.c prints [E:b1f1] at the start of the boot)
    printf("[E:fs0f1]\n");
}

// fs init: "n" = find a disk (or use the temporary file system) and create the base files, "p" = print the current directory
// "d" = demo mode: like "n" but the disks are never touched, so nothing on the computer can be changed
// boot.c calls fs_init("n") or fs_init("d") depending on the boot menu choice. Always returns 0
int fs_init (const char* arg) {
    bool demo = StrCompare(arg, "d");
    if (StrCompare(arg, "n") || demo) {
        // look for a AEROS disk and use it if there is one (demo mode never touches the disks)
        // (&& stops early, so in demo mode fat_mount_auto() is never even called)
        if (!demo && fat_mount_auto(fs_disk_status, sizeof(fs_disk_status))) {
            fs_mode = FS_MODE_DISK;
            printf("[ OK ] Disk: %s\n", fs_disk_status);
            printf("< INIT DISK FS .. >\n");
        } else {
            fs_mode = FS_MODE_MEMORY;
            if (demo) {
                printf("[INFO] Demo mode: disks are not loaded, files will not be saved\n");
            } else {
                // fs_disk_status says why, e.g. "no AEROS disk found" or "primary master is not labelled AEROS"
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

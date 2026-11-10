/*
    apps.c - the apps and files that come with AerOS

    The Makefile wraps every file in apps/ and docs/ in an object file with "ld -b binary". The
    linker then gives us two symbols per file, _binary_<folder>_<name>_start and _end (with
    the dots and slashes of the path turned into _), marking where its bytes sit inside the
    kernel. To add a file: put it in apps/ or docs/, add an EMBEDDED() line and a table entry.

    When AerOS starts, apps_install() copies each file to the disk (or the VFS):
      - the manual in /docs/ whenever the built-in copy is different, so it always describes
        the version of AerOS that is running
      - apps and system files when they are missing, or when a newer AerOS brings a new version
        and the copy on the disk is still the one AerOS put there. One you have changed is
        yours and is never replaced.

    To tell the two apart, /sys/apps.lst records the SHA-256 of each file as it was installed,
    one line per file:

        /bin/viewer.aer 3f9a...e1 (64 hex digits)

    At boot every app on the disk is compared with the built-in copy and with that record:

        disk == built-in             up to date, nothing to do
        disk == recorded             untouched since AerOS installed it: replace with built-in
        anything else                changed by you (or installed before records were kept,
                                     so there is no way to know): keep it

    To go back to the built-in copy of an app you have changed, delete it and restart.
*/

#include "apps.h"
#include "f_sys.h"
#include "../lib/primary_definitions.h"
#include "../lib/crypto.h"
#include "../lib/aer_stdio.h"

// declare the start and end symbols the linker made for one built-in file
#define EMBEDDED(symbol) \
    extern const char _binary_##symbol##_start[]; \
    extern const char _binary_##symbol##_end[];

// a table entry: the folder and name it is copied to, where it is, and whether to refresh it
#define FILE_ENTRY(dir, name, symbol, refresh) \
    { dir, name, _binary_##symbol##_start, _binary_##symbol##_end, refresh }

EMBEDDED(apps_hello_aer)
EMBEDDED(apps_guess_aer)
EMBEDDED(apps_viewer_aer)
EMBEDDED(apps_read_aer)
EMBEDDED(apps_sysinfo_aer)
EMBEDDED(apps_anim_aer)
EMBEDDED(apps_move_aer)
EMBEDDED(apps_sample_mpg)
EMBEDDED(apps_aeros_bmp)
EMBEDDED(apps_colours_bmp)
EMBEDDED(apps_startup_wav)
EMBEDDED(docs_index_md)
EMBEDDED(docs_start_md)
EMBEDDED(docs_commands_md)
EMBEDDED(docs_files_md)
EMBEDDED(docs_editor_md)
EMBEDDED(docs_aerlang_md)
EMBEDDED(docs_reader_md)
EMBEDDED(docs_security_md)
EMBEDDED(docs_sound_md)
EMBEDDED(docs_inside_md)
EMBEDDED(docs_video_md)
EMBEDDED(docs_sample_htm)

static const struct {
    const char* dir;
    const char* name;
    const char* start;
    const char* end;
    bool refresh; // replace the copy on the disk when it differs from the built-in one
} files[] = {
    FILE_ENTRY("/bin/", "hello.aer", apps_hello_aer, false),
    FILE_ENTRY("/bin/", "guess.aer", apps_guess_aer, false),
    FILE_ENTRY("/bin/", "viewer.aer", apps_viewer_aer, false),
    FILE_ENTRY("/bin/", "read.aer", apps_read_aer, false),
    FILE_ENTRY("/bin/", "sysinfo.aer", apps_sysinfo_aer, false),
    FILE_ENTRY("/bin/", "anim.aer", apps_anim_aer, false),
    FILE_ENTRY("/bin/", "move.aer", apps_move_aer, false),
    FILE_ENTRY("/bin/", "sample.mpg", apps_sample_mpg, false),
    FILE_ENTRY("/bin/", "aeros.bmp", apps_aeros_bmp, false),
    FILE_ENTRY("/bin/", "colours.bmp", apps_colours_bmp, false),
    FILE_ENTRY("/sys/", "startup.wav", apps_startup_wav, false),
    FILE_ENTRY("/docs/", "index.md", docs_index_md, true),
    FILE_ENTRY("/docs/", "start.md", docs_start_md, true),
    FILE_ENTRY("/docs/", "commands.md", docs_commands_md, true),
    FILE_ENTRY("/docs/", "files.md", docs_files_md, true),
    FILE_ENTRY("/docs/", "editor.md", docs_editor_md, true),
    FILE_ENTRY("/docs/", "aerlang.md", docs_aerlang_md, true),
    FILE_ENTRY("/docs/", "reader.md", docs_reader_md, true),
    FILE_ENTRY("/docs/", "security.md", docs_security_md, true),
    FILE_ENTRY("/docs/", "sound.md", docs_sound_md, true),
    FILE_ENTRY("/docs/", "inside.md", docs_inside_md, true),
    FILE_ENTRY("/docs/", "video.md", docs_video_md, true),
    FILE_ENTRY("/docs/", "sample.htm", docs_sample_htm, true),
};

#define FILE_COUNT (sizeof(files) / sizeof(files[0]))

// is the copy on the disk exactly the built-in one?
static bool same_as_disk (unsigned int i, uint32_t length) {
    uint32_t size = 0;
    char* data = fs_read_bytes(files[i].dir, files[i].name, &size);
    bool same = data && size == length && memcmp(data, files[i].start, length) == 0;
    free(data);
    return same;
}

// where the record of installed versions is kept (see the top of this file)
#define RECORD_DIR "/sys/"
#define RECORD_NAME "apps.lst"
#define HASH_HEX (SHA256_SIZE * 2)                 // a SHA-256 written out in hex
#define RECORD_LINE 96                             // a path, a space, the hash and a newline

// SHA-256 of some bytes, written as 64 lowercase hex digits
static void hash_hex (const void* data, uint32_t length, char out[HASH_HEX + 1]) {
    static const char digits[] = "0123456789abcdef";
    uint8_t hash[SHA256_SIZE];
    sha256(data, length, hash);
    for (int i = 0; i < SHA256_SIZE; i++) {
        out[i * 2] = digits[hash[i] >> 4];
        out[i * 2 + 1] = digits[hash[i] & 0x0F];
    }
    out[HASH_HEX] = '\0';
}

// what the copy on the disk is
enum { DISK_UNREADABLE, DISK_SAME, DISK_DIFFERENT };

// compare the copy on the disk with the built-in one, and hash it (into out) when it differs.
// Comparing first means an app that is up to date, the usual case, is never hashed
static int disk_copy (unsigned int i, uint32_t length, char out[HASH_HEX + 1]) {
    uint32_t size = 0;
    char* data = fs_read_bytes(files[i].dir, files[i].name, &size);
    if (!data) {
        return DISK_UNREADABLE;
    }
    int what = DISK_SAME;
    if (size != length || memcmp(data, files[i].start, length) != 0) {
        hash_hex(data, size, out);
        what = DISK_DIFFERENT;
    }
    free(data);
    return what;
}

// look up a path's line in the record, false if it has none (or there is no record)
static bool recorded_hash (const char* record, const char* path, char out[HASH_HEX + 1]) {
    size_t path_length = slen(path);
    const char* line = record;
    while (line && *line) {
        if (strncmp(line, path, path_length) == 0 && line[path_length] == ' ') {
            const char* hash = line + path_length + 1;
            if (strnlen(hash, HASH_HEX) < HASH_HEX) {
                return false;
            }
            memcpy(out, hash, HASH_HEX);
            out[HASH_HEX] = '\0';
            return true;
        }
        line = strchr(line, '\n');
        if (line) {
            line++;
        }
    }
    return false;
}

// install or update one app or system file, returning what the record should say about it:
// the hash of the version AerOS put on the disk, or "" when it isn't one of AerOS's versions
// (a copy you changed). *changed is set when the file on the disk was written
static void install_app (unsigned int i, const char* record, char keep[HASH_HEX + 1], bool* changed) {
    uint32_t length = (uint32_t)(files[i].end - files[i].start);
    char path[RECORD_LINE];
    char builtin[HASH_HEX + 1], ondisk[HASH_HEX + 1], recorded[HASH_HEX + 1];
    sprintf(path, "%s%s", files[i].dir, files[i].name);
    hash_hex(files[i].start, length, builtin);
    keep[0] = '\0';
    *changed = false;

    bool has_record = recorded_hash(record, path, recorded);

    if (!fs_file_exists(files[i].dir, files[i].name)) {
        // missing (a new disk, a new app, or one you deleted to get the built-in copy back)
        if (fs_create_bytes(files[i].dir, files[i].name, files[i].start, length) == FS_OK) {
            strcpy(keep, builtin);
            *changed = true;
        }
        return;
    }

    int disk = disk_copy(i, length, ondisk);
    if (disk == DISK_UNREADABLE) {
        // can't read it: leave the file and what we knew about it as they are
        if (has_record) {
            strcpy(keep, recorded);
        }
    } else if (disk == DISK_SAME) {
        strcpy(keep, builtin); // up to date
    } else if (has_record && scmp(ondisk, recorded) == 0) {
        // an older version AerOS installed, never changed since: bring it up to date
        if (fs_write_bytes(files[i].dir, files[i].name, files[i].start, length, false) == FS_OK) {
            strcpy(keep, builtin);
            *changed = true;
        } else {
            strcpy(keep, recorded);
        }
    }
    // otherwise it's your own version: kept, and not recorded as one of AerOS's
}

int apps_install (void) {
    int installed = 0;

    // what was installed last time (NULL on a new disk, or one from before records were kept)
    char* old_record = fs_read_f(RECORD_DIR, RECORD_NAME);
    char new_record[FILE_COUNT * RECORD_LINE + 1];
    new_record[0] = '\0';

    for (unsigned int i = 0; i < FILE_COUNT; i++) {
        uint32_t length = (uint32_t)(files[i].end - files[i].start);

        if (files[i].refresh) {
            // the manual: always the built-in copy
            int result;
            if (!fs_file_exists(files[i].dir, files[i].name)) {
                result = fs_create_bytes(files[i].dir, files[i].name, files[i].start, length);
            } else if (!same_as_disk(i, length)) {
                result = fs_write_bytes(files[i].dir, files[i].name, files[i].start, length, false);
            } else {
                continue;
            }
            if (result == FS_OK) {
                installed++;
            }
            continue;
        }

        char keep[HASH_HEX + 1];
        bool changed;
        install_app(i, old_record, keep, &changed);
        if (changed) {
            installed++;
        }
        if (keep[0]) {
            char line[RECORD_LINE];
            sprintf(line, "%s%s %s\n", files[i].dir, files[i].name, keep);
            strcat(new_record, line);
        }
    }

    // only write the record when it says something new
    if (!old_record || scmp(old_record, new_record) != 0) {
        if (fs_file_exists(RECORD_DIR, RECORD_NAME)) {
            fs_write_f(RECORD_DIR, RECORD_NAME, new_record, false);
        } else {
            fs_create_f(RECORD_DIR, RECORD_NAME, new_record);
        }
    }
    free(old_record);
    return installed;
}

const void* apps_builtin (const char* name, uint32_t* size) {
    for (unsigned int i = 0; i < FILE_COUNT; i++) {
        if (seq(files[i].name, name)) {
            *size = (uint32_t)(files[i].end - files[i].start);
            return files[i].start;
        }
    }
    return 0;
}

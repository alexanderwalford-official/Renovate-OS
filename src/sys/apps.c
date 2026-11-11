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

    Apps live in /bin/, and games in /bin/games/ (both are searched when an app is run by its
    name, see aer_app_folders in lang/aerlang.h). When an app moves into a folder of its own,
    an untouched old copy left in /bin/ by an older AerOS is deleted (tidy_old_copy).
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
EMBEDDED(docs_why_md)
EMBEDDED(docs_shell_md)
EMBEDDED(docs_versions_md)
EMBEDDED(docs_perms_md)
EMBEDDED(docs_network_md)
EMBEDDED(docs_rt_md)
EMBEDDED(docs_text_md)
EMBEDDED(docs_sample_htm)
EMBEDDED(apps_sleep_aer)
EMBEDDED(apps_snake_aer)

// every built-in file, where it goes and where its bytes are. Apps and system files (refresh
// false) follow the record rules at the top of this file, the manual (refresh true) is simply
// kept the same as the built-in copy
static const struct {
    const char* dir;
    const char* name;
    const char* start; // the first byte of the built-in copy
    const char* end;   // one past the last byte, so end - start is its size
    bool refresh; // replace the copy on the disk when it differs from the built-in one
} files[] = {
    FILE_ENTRY("/bin/", "hello.aer", apps_hello_aer, false),
    FILE_ENTRY("/bin/games/", "guess.aer", apps_guess_aer, false),
    FILE_ENTRY("/bin/", "viewer.aer", apps_viewer_aer, false),
    FILE_ENTRY("/bin/", "read.aer", apps_read_aer, false),
    FILE_ENTRY("/bin/", "sysinfo.aer", apps_sysinfo_aer, false),
    FILE_ENTRY("/bin/", "anim.aer", apps_anim_aer, false),
    FILE_ENTRY("/bin/", "sleep.aer", apps_sleep_aer, false),
    FILE_ENTRY("/bin/games/", "snake.aer", apps_snake_aer, false),
    FILE_ENTRY("/bin/games/", "move.aer", apps_move_aer, false),
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
    FILE_ENTRY("/docs/", "why.md", docs_why_md, true),
    FILE_ENTRY("/docs/", "shell.md", docs_shell_md, true),
    FILE_ENTRY("/docs/", "versions.md", docs_versions_md, true),
    FILE_ENTRY("/docs/", "perms.md", docs_perms_md, true),
    FILE_ENTRY("/docs/", "network.md", docs_network_md, true),
    FILE_ENTRY("/docs/", "rt.md", docs_rt_md, true),
    FILE_ENTRY("/docs/", "text.md", docs_text_md, true),
    FILE_ENTRY("/docs/", "sample.htm", docs_sample_htm, true),
};

#define FILE_COUNT (sizeof(files) / sizeof(files[0]))

// is the copy on the disk exactly the built-in one?
// i = the file's place in files[], length = the size of its built-in copy
static bool same_as_disk (unsigned int i, uint32_t length) {
    uint32_t size = 0;
    char* data = fs_read_bytes(files[i].dir, files[i].name, &size);
    // a file that can't be read counts as different. The size is compared first, so memcmp
    // never reads past the end of a shorter file
    bool same = data && size == length && memcmp(data, files[i].start, length) == 0;
    free(data);
    return same;
}

// where the record of installed versions is kept (see the top of this file)
#define RECORD_DIR "/sys/"
#define RECORD_NAME "apps.lst"
#define HASH_HEX (SHA256_SIZE * 2)                 // a SHA-256 written out in hex
#define RECORD_LINE 96                             // a path, a space, the hash and a newline
// (the hash and the space, newline and '\0' take 67 of the 96, which leaves 29 for the path)

// SHA-256 of some bytes, written as 64 lowercase hex digits
static void hash_hex (const void* data, uint32_t length, char out[HASH_HEX + 1]) {
    static const char digits[] = "0123456789abcdef";
    uint8_t hash[SHA256_SIZE];
    sha256(data, length, hash);
    // each byte becomes two hex digits: >> 4 picks out its top 4 bits (the first digit) and
    // & 0x0F its bottom 4 bits (the second), each a number from 0 to 15 to look up in digits
    for (int i = 0; i < SHA256_SIZE; i++) {
        out[i * 2] = digits[hash[i] >> 4];
        out[i * 2 + 1] = digits[hash[i] & 0x0F];
    }
    out[HASH_HEX] = '\0';
}

// what the copy on the disk is (what disk_copy() found)
//   DISK_UNREADABLE  it is there but couldn't be read
//   DISK_SAME        exactly the built-in copy
//   DISK_DIFFERENT   anything else, and its hash has been worked out
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
    // || stops at a different size, so memcmp only runs when both are the same length
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
    // record is NULL when there is no record at all, which the loop treats as an empty one
    const char* line = record;
    while (line && *line) {
        // the line must start with the whole path followed by a space, so "/bin/a.aer" doesn't
        // match a line for "/bin/a.aer2"
        if (strncmp(line, path, path_length) == 0 && line[path_length] == ' ') {
            const char* hash = line + path_length + 1;
            // a record cut short (fewer than 64 characters left) has no proper hash, so it
            // counts as no record, rather than reading past the end
            if (strnlen(hash, HASH_HEX) < HASH_HEX) {
                return false;
            }
            memcpy(out, hash, HASH_HEX);
            out[HASH_HEX] = '\0';
            return true;
        }
        // on to the start of the next line (strchr gives NULL after the last line, which ends the loop)
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
    // the full path, the way it is written in the record ("/bin/hello.aer")
    sprintf(path, "%s%s", files[i].dir, files[i].name);
    hash_hex(files[i].start, length, builtin);
    // start from "record nothing, nothing written", each case below changes what it needs to
    keep[0] = '\0';
    *changed = false;

    bool has_record = recorded_hash(record, path, recorded);

    // the cases below follow the table at the top of this file
    if (!fs_file_exists(files[i].dir, files[i].name)) {
        // missing (a new disk, a new app, or one you deleted to get the built-in copy back)
        if (fs_create_bytes(files[i].dir, files[i].name, files[i].start, length) == FS_OK) {
            strcpy(keep, builtin);
            *changed = true;
        }
        // if the create failed keep stays "" (nothing to record), and as the file is still
        // missing the next boot simply tries again
        return;
    }

    // ondisk is only filled in when the copy is DISK_DIFFERENT
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
            // the write failed, so the old version is still there: keep its record, and it
            // will be tried again next boot
            strcpy(keep, recorded);
        }
    }
    // otherwise it's your own version: kept, and not recorded as one of AerOS's
}

// An app that has moved into a folder of its own (the games, into /bin/games/) may still have
// its old copy in /bin/ on a disk from an older AerOS. If that copy is one AerOS put there and
// nobody has changed (it matches the record, or the built-in one), it goes. One you have
// changed is yours, so it stays (and, as /bin/ is looked in first, it is the one that runs)
static void tidy_old_copy (unsigned int i, const char* record) {
    const char* old_dir = "/bin/";
    // only files in a folder inside /bin/ ("/bin/games/"): one that lives in /bin/ itself, or
    // somewhere else entirely (/sys/, /docs/), has no old copy to tidy up
    if (seq(files[i].dir, old_dir) || strncmp(files[i].dir, old_dir, slen(old_dir)) != 0) {
        return; // not an app that moved out of /bin/
    }
    uint32_t size = 0;
    char* data = fs_read_bytes(old_dir, files[i].name, &size);
    if (data == NULL) {
        return; // no old copy
    }
    // hash the old copy and the built-in one, so they can be compared with each other and with
    // the record. The old copy's bytes aren't needed after that
    char path[RECORD_LINE];
    char ondisk[HASH_HEX + 1], recorded[HASH_HEX + 1], builtin[HASH_HEX + 1];
    sprintf(path, "%s%s", old_dir, files[i].name);
    hash_hex(data, size, ondisk);
    hash_hex(files[i].start, (uint32_t)(files[i].end - files[i].start), builtin);
    free(data);

    // untouched if it is the current built-in copy, or the version the record says AerOS put
    // there. (The old /bin/ line then drops out of the record, as nothing writes it again)
    bool untouched = scmp(ondisk, builtin) == 0
                     || (recorded_hash(record, path, recorded) && scmp(ondisk, recorded) == 0);
    if (untouched) {
        fs_delete_f(old_dir, files[i].name);
    }
}

// copy the built-in files to the disk where needed (see the top of this file), returns how many were written
int apps_install (void) {
    int installed = 0;

    // what was installed last time (NULL on a new disk, or one from before records were kept)
    char* old_record = fs_read_f(RECORD_DIR, RECORD_NAME);
    // the new record is built up from scratch, one line per file, so a file that is no longer
    // AerOS's version (or no longer built in) simply drops out of it. Room for a full line
    // for every file, + 1 for the '\0'
    char new_record[FILE_COUNT * RECORD_LINE + 1];
    new_record[0] = '\0';

    for (unsigned int i = 0; i < FILE_COUNT; i++) {
        uint32_t length = (uint32_t)(files[i].end - files[i].start);

        // the manual isn't in the record: it is always replaced, so there is nothing to remember
        if (files[i].refresh) {
            // the manual: always the built-in copy
            int result;
            if (!fs_file_exists(files[i].dir, files[i].name)) {
                result = fs_create_bytes(files[i].dir, files[i].name, files[i].start, length);
            } else if (!same_as_disk(i, length)) {
                result = fs_write_bytes(files[i].dir, files[i].name, files[i].start, length, false);
            } else {
                continue; // already the same, nothing to write
            }
            if (result == FS_OK) {
                installed++;
            }
            continue;
        }

        // an app or system file: tidy away an old copy left in /bin/ first, then install or update it
        char keep[HASH_HEX + 1];
        bool changed;
        tidy_old_copy(i, old_record);
        install_app(i, old_record, keep, &changed);
        if (changed) {
            installed++;
        }
        // keep is "" for a copy you changed, which then gets no line in the record
        if (keep[0]) {
            char line[RECORD_LINE];
            sprintf(line, "%s%s %s\n", files[i].dir, files[i].name, keep);
            strcat(new_record, line);
        }
    }

    // only write the record when it says something new
    // (so a normal boot, with everything up to date, writes nothing to the disk at all)
    if (!old_record || scmp(old_record, new_record) != 0) {
        // write needs an existing file, so the first time round it is created instead
        if (fs_file_exists(RECORD_DIR, RECORD_NAME)) {
            fs_write_f(RECORD_DIR, RECORD_NAME, new_record, false);
        } else {
            fs_create_f(RECORD_DIR, RECORD_NAME, new_record);
        }
    }
    // free(NULL) does nothing, so this is safe when there was no old record
    free(old_record);
    return installed;
}

// the built-in copy of a file by its name ("hello.aer"), and its size in *size. 0 (NULL) if there is none
const void* apps_builtin (const char* name, uint32_t* size) {
    // matched by name alone, not folder, so the first file in the table with that name wins
    for (unsigned int i = 0; i < FILE_COUNT; i++) {
        if (seq(files[i].name, name)) {
            *size = (uint32_t)(files[i].end - files[i].start);
            return files[i].start;
        }
    }
    return 0;
}

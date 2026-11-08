/*
    apps.c - the apps and files that come with AerOS

    The Makefile wraps every file in apps/ and docs/ in an object file with "ld -b binary". The
    linker then gives us two symbols per file, _binary_<folder>_<name>_start and _end (with
    the dots and slashes of the path turned into _), marking where its bytes sit inside the
    kernel. To add a file: put it in apps/ or docs/, add an EMBEDDED() line and a table entry.

    When AerOS starts, apps_install() copies each file to the disk (or the VFS):
      - apps and system files only when they are missing, so your own changes are kept
      - the manual in /docs/ whenever the built-in copy is different, so it always describes
        the version of AerOS that is running
*/

#include "apps.h"
#include "f_sys.h"
#include "../lib/primary_definitions.h"

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

int apps_install (void) {
    int installed = 0;
    for (unsigned int i = 0; i < FILE_COUNT; i++) {
        uint32_t length = (uint32_t)(files[i].end - files[i].start);
        int result;
        if (!fs_file_exists(files[i].dir, files[i].name)) {
            result = fs_create_bytes(files[i].dir, files[i].name, files[i].start, length);
        } else if (files[i].refresh && !same_as_disk(i, length)) {
            result = fs_write_bytes(files[i].dir, files[i].name, files[i].start, length, false);
        } else {
            continue; // already there (and the user's to change)
        }
        if (result == FS_OK) {
            installed++;
        }
    }
    return installed;
}

const void* apps_builtin (const char* name, uint32_t* size) {
    for (unsigned int i = 0; i < FILE_COUNT; i++) {
        if (StrCompare(files[i].name, name)) {
            *size = (uint32_t)(files[i].end - files[i].start);
            return files[i].start;
        }
    }
    return 0;
}

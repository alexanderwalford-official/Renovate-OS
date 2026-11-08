/*
    apps.c - the apps and files that come with AerOS

    The Makefile wraps every file in apps/ in an object file with "ld -b binary". The linker then
    gives us two symbols per file, _binary_apps_<name>_start and _end, marking where its bytes
    sit inside the kernel. To add a file: put it in apps/ and add a line to the table below, with
    the folder it should be copied into.
*/

#include "apps.h"
#include "f_sys.h"
#include "../lib/primary_definitions.h"

#define APP(symbol) \
    extern const char _binary_apps_##symbol##_start[]; \
    extern const char _binary_apps_##symbol##_end[];

APP(hello_ren)
APP(guess_ren)
APP(viewer_ren)
APP(aeros_bmp)
APP(colours_bmp)
APP(startup_wav)

static const struct {
    const char* dir;
    const char* name;
    const char* start;
    const char* end;
} apps[] = {
    { "/bin/", "hello.ren", _binary_apps_hello_ren_start, _binary_apps_hello_ren_end },
    { "/bin/", "guess.ren", _binary_apps_guess_ren_start, _binary_apps_guess_ren_end },
    { "/bin/", "viewer.ren", _binary_apps_viewer_ren_start, _binary_apps_viewer_ren_end },
    { "/bin/", "aeros.bmp", _binary_apps_aeros_bmp_start, _binary_apps_aeros_bmp_end },
    { "/bin/", "colours.bmp", _binary_apps_colours_bmp_start, _binary_apps_colours_bmp_end },
    { "/sys/", "startup.wav", _binary_apps_startup_wav_start, _binary_apps_startup_wav_end },
};

#define APP_COUNT (sizeof(apps) / sizeof(apps[0]))

int apps_install (void) {
    int installed = 0;
    for (unsigned int i = 0; i < APP_COUNT; i++) {
        if (fs_file_exists(apps[i].dir, apps[i].name)) {
            continue;
        }
        uint32_t length = (uint32_t)(apps[i].end - apps[i].start);
        if (fs_create_bytes(apps[i].dir, apps[i].name, apps[i].start, length) == FS_OK) {
            installed++;
        }
    }
    return installed;
}

const void* apps_builtin (const char* name, uint32_t* size) {
    for (unsigned int i = 0; i < APP_COUNT; i++) {
        if (StrCompare(apps[i].name, name)) {
            *size = (uint32_t)(apps[i].end - apps[i].start);
            return apps[i].start;
        }
    }
    return 0;
}

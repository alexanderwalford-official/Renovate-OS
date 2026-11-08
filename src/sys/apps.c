/*
    apps.c - the apps that come with AerOS

    The Makefile wraps every file in apps/ in an object file with "ld -b binary". The linker then
    gives us two symbols per file, _binary_apps_<name>_start and _end, marking where its bytes
    sit inside the kernel. To add an app: put the file in apps/ and add a line to the table below.
*/

#include "apps.h"
#include <stdint.h>
#include "f_sys.h"

#define APP(symbol) \
    extern const char _binary_apps_##symbol##_start[]; \
    extern const char _binary_apps_##symbol##_end[];

APP(hello_ren)
APP(guess_ren)
APP(viewer_ren)
APP(aeros_bmp)
APP(colours_bmp)

static const struct {
    const char* name;
    const char* start;
    const char* end;
} apps[] = {
    { "hello.ren", _binary_apps_hello_ren_start, _binary_apps_hello_ren_end },
    { "guess.ren", _binary_apps_guess_ren_start, _binary_apps_guess_ren_end },
    { "viewer.ren", _binary_apps_viewer_ren_start, _binary_apps_viewer_ren_end },
    { "aeros.bmp", _binary_apps_aeros_bmp_start, _binary_apps_aeros_bmp_end },
    { "colours.bmp", _binary_apps_colours_bmp_start, _binary_apps_colours_bmp_end },
};

int apps_install (void) {
    int installed = 0;
    for (unsigned int i = 0; i < sizeof(apps) / sizeof(apps[0]); i++) {
        if (fs_file_exists("/bin/", apps[i].name)) {
            continue;
        }
        uint32_t length = (uint32_t)(apps[i].end - apps[i].start);
        if (fs_create_bytes("/bin/", apps[i].name, apps[i].start, length) == FS_OK) {
            installed++;
        }
    }
    return installed;
}

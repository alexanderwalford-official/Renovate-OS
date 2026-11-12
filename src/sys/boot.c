/*
    boot.c - the boot sequence

    Two jobs live here:

    1. Reading the multiboot information. When GRUB starts a multiboot kernel it leaves the
       magic number 0x2BADB002 in eax and the address of a "multiboot information" structure
       in ebx. That structure describes the machine: how much memory there is, the boot
       device, the kernel command line, modules, a full memory map and more. Bit N of its
       flags field says whether the Nth group of fields is filled in. We use three of them:
       the memory sizes, the memory map (the biggest free stretch of RAM becomes extra heap)
       and the framebuffer GRUB switched the screen to (entry.asm asks for a 1024 x 768 x 32
       graphics mode in the multiboot header).
       https://www.gnu.org/software/grub/manual/multiboot/multiboot.html#Boot-information-format
       https://wiki.osdev.org/Multiboot

    2. init_boot(), run as the "init_boot" process after the splash screen. It prints the boot
       log: one status line per piece of hardware set up in kernel.c, using
           [ OK ]  it worked
           [FAIL]  it was not set up (the matching *_ready flag is still false)
           [WARN]  it is missing but we can carry on without it
           [INFO]  just so you know (printed by the file system)
       then checks the cryptography against its test vectors, looks for a sound card, starts
       the file system, copies the built-in apps and files onto it and gives the user a moment
       to read it all.
*/

#include "boot.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/input/IO.h"
#include "../lib/aer_stdlib.h"
#include "../lib/aer_stdio.h"
#include "../lib/aer_string.h"
#include "interrupts.h"
#include "sys_clck.h"
#include "f_sys.h"
#include "apps.h"
#include "../lib/crypto.h"
#include "../drivers/audio/ac97.h"
#include "net.h"
#include "rt.h"

// the value a multiboot bootloader leaves in eax (not the same as the 0x1BADB002 in our header)
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002
// bit 0 of the info flags: mem_lower and mem_upper are valid
#define MULTIBOOT_INFO_MEMORY      0x00000001
#define BOOT_LOG_DELAY             2000 // how long the boot log stays on screen (ms)

// the start of the information structure GRUB hands to the kernel
// the real structure carries on with many more fields (boot_device, cmdline, mods, mmap ...),
// we only read through a pointer, so declaring just the start is fine
typedef struct __attribute__((packed)) multiboot_info {
    uint32_t flags; // which of the fields below are valid
    uint32_t mem_lower; // KB of memory below 1MB
    uint32_t mem_upper; // KB of memory above 1MB
    uint32_t boot_device;
    uint32_t cmdline; // the command line (flags bit 2): the address of a '\0' terminated string
    uint32_t unused1[6]; // modules, symbols
    uint32_t mmap_length; // the memory map (flags bit 6): its size in bytes
    uint32_t mmap_addr;   // and where it is
    uint32_t unused2[9];  // drives, config table, bootloader name, APM, VBE
    uint64_t framebuffer_addr; // from here on: the screen GRUB set up (flags bit 12)
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type; // 0 = palette, 1 = RGB pixels, 2 = VGA text mode
    // gotcha: the multiboot spec says the colour layout starts at offset 110, but GRUB declares it
    // in a union with a 32 bit field, so the compiler aligned it to 112 and that is where GRUB writes it
    uint8_t padding[2];
    uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
} multiboot_info_t;

// bit 2 of the info flags: the command line field is valid
#define MULTIBOOT_INFO_CMDLINE     0x00000004
// bit 6 of the info flags: the memory map fields are valid
#define MULTIBOOT_INFO_MEMORY_MAP  0x00000040
#define MULTIBOOT_MEMORY_AVAILABLE 1

// one entry of the memory map. "size" doesn't count itself, the next entry starts size + 4 later
typedef struct __attribute__((packed)) {
    uint32_t size;
    uint64_t base;
    uint64_t length;
    uint32_t type; // 1 = free RAM, anything else is reserved, ACPI tables, broken ...
} multiboot_mmap_entry_t;

extern char end[]; // the first byte after the kernel (linker.ld)

uint32_t boot_heap_extra_kb = 0;

// give the heap the biggest stretch of free RAM above the kernel (the built-in 4MB is only enough
// to get going: pictures, programs and their data need far more)
static void boot_add_memory (const multiboot_info_t* info) {
    // the biggest usable stretch found so far (64 bit, as the map can describe memory past 4GB)
    uint64_t best_start = 0, best_end = 0;
    // walk the entries one by one. They are stepped over by their own size field rather than
    // sizeof, because a bootloader is allowed to make the entries bigger than this struct
    uint32_t offset = 0;
    while (offset + sizeof(multiboot_mmap_entry_t) <= info->mmap_length) {
        const multiboot_mmap_entry_t* e = (const multiboot_mmap_entry_t*)(uintptr_t)(info->mmap_addr + offset);
        offset += e->size + 4;
        if (e->type != MULTIBOOT_MEMORY_AVAILABLE) {
            continue;
        }
        // only the part above the kernel and below 4GB (no paging, so nothing higher can be reached)
        uint64_t start = e->base;
        uint64_t stop = e->base + e->length;
        if (start < (uintptr_t)end) {
            start = (uintptr_t)end;
        }
        if (stop > 0x100000000ULL) {
            stop = 0x100000000ULL;
        }
        // keep it if anything is left after the trimming and it beats the best so far
        if (stop > start && stop - start > best_end - best_start) {
            best_start = start;
            best_end = stop;
        }
    }
    // only worth adding if it is at least 1MB (on a tiny machine there may be nothing past the kernel)
    if (best_end - best_start >= 1024 * 1024) {
        heap_add_region((void*)(uintptr_t)best_start, (size_t)(best_end - best_start));
        boot_heap_extra_kb = (uint32_t)((best_end - best_start) / 1024);
    }
}

// bit 12 of the info flags: the framebuffer fields are valid
#define MULTIBOOT_INFO_FRAMEBUFFER 0x00001000
#define MULTIBOOT_FRAMEBUFFER_RGB  1

// the boot options menu (sys/recovery.c) can change this before init_boot() runs
int boot_option = BOOT_OPT_NORMAL;
// set by the splash screen if X is pressed during its 4 second window
bool boot_options_requested = false;

// filled in by boot_parse_multiboot(), all stay false / 0 if we were not started by multiboot
bool boot_multiboot_valid = false;
uint32_t sys_mem_lower_kb = 0; // conventional memory, at most 640 KB
uint32_t sys_mem_upper_kb = 0; // memory from 1MB up to the first hole in the memory map
bool boot_framebuffer_valid = false; // GRUB gave us a graphics framebuffer instead of text mode
vga_framebuffer_t boot_framebuffer;

// Is `word` one of the words (split by spaces) in the bootloader's command line? The command line
// is whatever follows the kernel in the bootloader's settings: QEMU passes "-append fast" on, which
// is how the tests in tools/test/ skip the splash screen's wait and the boot log's pause. It
// usually starts with the kernel's own path, which can never match a word like "fast"
static bool boot_has_word (const char* line, const char* word) {
    size_t length = slen(word);
    while (*line != '\0') {
        // skip the spaces before a word, then measure it
        while (*line == ' ') {
            line++;
        }
        size_t size = 0;
        while (line[size] != '\0' && line[size] != ' ') {
            size++;
        }
        if (size == length && strncmp(line, word, length) == 0) {
            return true;
        }
        line += size;
    }
    return false;
}

// read what the bootloader told us about the machine
// magic = eax from GRUB, info_addr = ebx from GRUB (the physical address of the structure)
// called very early in kernel.c, before anything else could reuse the memory GRUB put the structure in
void boot_parse_multiboot (uint32_t magic, uint32_t info_addr) {
    // without the right magic number, ebx could hold anything, so don't touch it
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC || info_addr == 0) {
        return;
    }
    // there is no paging, so the physical address GRUB gave us can be used as a pointer directly
    const multiboot_info_t* info = (const multiboot_info_t*)(uintptr_t)info_addr;
    boot_multiboot_valid = true;
    // only trust the memory sizes if GRUB says it filled them in
    if (info->flags & MULTIBOOT_INFO_MEMORY) {
        sys_mem_lower_kb = info->mem_lower;
        sys_mem_upper_kb = info->mem_upper;
    }
    // the word "fast" on the command line boots straight to the prompt (see boot_has_word)
    if ((info->flags & MULTIBOOT_INFO_CMDLINE) && info->cmdline != 0 &&
        boot_has_word((const char*)(uintptr_t)info->cmdline, "fast")) {
        boot_option = BOOT_OPT_FAST;
    }
    // a graphics framebuffer (UEFI PCs). Without paging we can only reach the first 4GB, so a
    // framebuffer above that can't be used and we stay with VGA text mode
    if ((info->flags & MULTIBOOT_INFO_FRAMEBUFFER) && info->framebuffer_type == MULTIBOOT_FRAMEBUFFER_RGB &&
        info->framebuffer_addr < 0x100000000ULL) {
        // copy it all out, since GRUB's structure may be written over later. The *_pos and
        // *_size pairs say which bits of a pixel hold each colour (e.g. red at bit 16, 8 bits)
        boot_framebuffer.address = (uint32_t)info->framebuffer_addr;
        boot_framebuffer.pitch = info->framebuffer_pitch;
        boot_framebuffer.width = info->framebuffer_width;
        boot_framebuffer.height = info->framebuffer_height;
        boot_framebuffer.bpp = info->framebuffer_bpp;
        boot_framebuffer.red_pos = info->red_pos;
        boot_framebuffer.red_size = info->red_size;
        boot_framebuffer.green_pos = info->green_pos;
        boot_framebuffer.green_size = info->green_size;
        boot_framebuffer.blue_pos = info->blue_pos;
        boot_framebuffer.blue_size = info->blue_size;
        boot_framebuffer_valid = true;
    }
    // last, because the heap may hand out the memory the multiboot information sits in
    if (info->flags & MULTIBOOT_INFO_MEMORY_MAP) {
        boot_add_memory(info);
    }
}

// the boot log
// shows whether each part of kernel.c's hardware setup worked, starts the file system and waits
// BOOT_LOG_DELAY ms so it can be read (skipped on fast boot). Returns 0
int init_boot (void) {
    // black text on a light grey screen, same as the splash screen
    colour(BLACK, BRIGHT);
    console_clear();

    prtf("[E:b1f1] AerOS boot sequence started\n\n");

    // each driver sets its *_ready flag at the end of its init function in kernel.c, so a [FAIL]
    // here means that step never ran (handy if someone reorders or removes a line in main())
    if (vga_framebuffer_active()) {
        prtf("[ OK ] Screen: %ux%u framebuffer, %u bits per pixel\n", boot_framebuffer.width,
               boot_framebuffer.height, (unsigned int)boot_framebuffer.bpp);
    } else {
        prtf("[ OK ] Screen: VGA text mode\n");
    }
    prtf("%s Descriptor tables & interrupts (GDT / IDT / PIC)\n", interrupts_ready ? "[ OK ]" : "[FAIL]");
    if (clck_ready) {
        prtf("[ OK ] System timer at %u Hz\n", (unsigned int)PIT_HZ);
    } else {
        prtf("[WARN] System timer is not ticking, using busy waits instead\n");
    }
    prtf("%s PS/2 keyboard\n", keyboard_ready ? "[ OK ]" : "[FAIL]");
    // only a warning, nothing needs the memory size yet (the heap is a fixed size array)
    if (boot_multiboot_valid) {
        prtf("[ OK ] Memory: %u KB lower, %u KB upper\n", sys_mem_lower_kb, sys_mem_upper_kb);
    } else {
        prtf("[WARN] No memory information from the bootloader\n");
    }
    prtf("[ OK ] Kernel heap: %u KB\n", (unsigned int)(heap_total() / 1024));

    // rtc_datetime() fills in now and returns it, so it can go straight into prtf
    char now[FS_DATETIME_MAX];
    prtf("[ OK ] Real time clock: %s\n", rtc_datetime(now, sizeof(now)));
    // check SHA-256, HMAC, PBKDF2 and AES against published test vectors before trusting them with passwords
    prtf("%s Crypto self-test (SHA-256, HMAC, PBKDF2, AES-256)\n", crypto_selftest() ? "[ OK ]" : "[FAIL]");
    // a missing sound card isn't a problem, only PLY needs one
    if (ac97_init()) {
        prtf("[ OK ] Sound: %s\n", ac97_name());
    } else {
        prtf("[WARN] No sound card found (AerOS supports AC'97), PLY won't work\n");
    }
    // the firewall is always on: nothing is sent or let in that a rule (FW ALLOW) doesn't allow
    if (net_init()) {
        prtf("[ OK ] Network: %s, firewall on\n\n", net_card_name());
    } else {
        prtf("[INFO] No network card found (AerOS supports the Intel e1000)\n\n");
    }
    // RT mode (picked in the boot options): the real-time tasks start ticking from here
    if (boot_option == BOOT_OPT_RT) {
        rt_start();
        prtf("[INFO] RT mode (experimental): %d real-time task(s), RT lists them\n\n", rt_count());
    }

    // initialise the file system (the disk, or the temporary one if there is no disk)
    // "n" = normal: mount a FAT disk labelled AEROS if there is one, otherwise use the VFS
    // "d" = demo: go straight to the VFS without probing the disks at all
    fs_init(boot_option == BOOT_OPT_DEMO ? "d" : "n");

    // the apps that come with AerOS, copied into /bin/ unless they are already there
    int installed = apps_install();
    if (installed > 0) {
        prtf("< APPS .. > %d built-in file(s) added or updated\n", installed);
    }

    // beep once
    //beep(0);

    // the file system is up (on disk or in memory), the terminal can use it from here on
    prtf("\n[VFS:UP]\n");

    // give the user time to read the log, unless fast boot was chosen
    if (boot_option != BOOT_OPT_FAST) {
        clck_hang(BOOT_LOG_DELAY);
    }

    return 0;
}

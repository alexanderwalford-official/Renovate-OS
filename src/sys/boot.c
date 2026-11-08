/*
    boot.c - the boot sequence

    Two jobs live here:

    1. Reading the multiboot information. When GRUB starts a multiboot kernel it leaves the
       magic number 0x2BADB002 in eax and the address of a "multiboot information" structure
       in ebx. That structure describes the machine: how much memory there is, the boot
       device, the kernel command line, modules, a full memory map and more. Bit N of its
       flags field says whether the Nth group of fields is filled in. We only need the memory
       sizes for now (entry.asm asks GRUB for them with the MEMINFO header flag).
       https://www.gnu.org/software/grub/manual/multiboot/multiboot.html#Boot-information-format
       https://wiki.osdev.org/Multiboot

    2. init_boot(), run as the "init_boot" process after the splash screen. It prints the boot
       log: one status line per piece of hardware set up in kernel.c, using
           [ OK ]  it worked
           [FAIL]  it was not set up (the matching *_ready flag is still false)
           [WARN]  it is missing but we can carry on without it
           [INFO]  just so you know (printed by the file system)
       then starts the file system and gives the user a moment to read it all.
*/

#include "boot.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/input/IO.h"
#include "../lib/ren_stdlib.h"
#include "../lib/ren_stdio.h"
#include "interrupts.h"
#include "sys_clck.h"
#include "f_sys.h"

// the value a multiboot bootloader leaves in eax (not the same as the 0x1BADB002 in our header)
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002
// bit 0 of the info flags: mem_lower and mem_upper are valid
#define MULTIBOOT_INFO_MEMORY      0x00000001
#define BOOT_LOG_DELAY             2000 // how long the boot log stays on screen (ms)

// the start of the information structure GRUB hands to the kernel
// the real structure carries on with many more fields (boot_device, cmdline, mods, mmap ...),
// we only read through a pointer, so declaring just the start is fine
typedef struct multiboot_info {
    uint32_t flags; // which of the fields below (and the ones we left out) are valid
    uint32_t mem_lower; // KB of memory below 1MB
    uint32_t mem_upper; // KB of memory above 1MB
} multiboot_info_t;

// the boot options menu (sys/recovery.c) can change this before init_boot() runs
int boot_option = BOOT_OPT_NORMAL;
// set by the splash screen if X is pressed during its 4 second window
bool boot_options_requested = false;

// filled in by boot_parse_multiboot(), all stay false / 0 if we were not started by multiboot
bool boot_multiboot_valid = false;
uint32_t sys_mem_lower_kb = 0; // conventional memory, at most 640 KB
uint32_t sys_mem_upper_kb = 0; // memory from 1MB up to the first hole in the memory map

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
}

// the boot log
// shows whether each part of kernel.c's hardware setup worked, starts the file system and waits
// BOOT_LOG_DELAY ms so it can be read (skipped on fast boot). Returns 0
int init_boot (void) {
    // black text on a light grey screen, same as the splash screen
    console_set_color(BLACK, BRIGHT);
    console_clear();

    printf("[E:b1f1] Renovate OS boot sequence started\n\n");

    // each driver sets its *_ready flag at the end of its init function in kernel.c, so a [FAIL]
    // here means that step never ran (handy if someone reorders or removes a line in main())
    printf("%s Descriptor tables & interrupts (GDT / IDT / PIC)\n", interrupts_ready ? "[ OK ]" : "[FAIL]");
    printf("%s System timer at %u Hz\n", clck_ready ? "[ OK ]" : "[FAIL]", (unsigned int)PIT_HZ);
    printf("%s PS/2 keyboard\n", keyboard_ready ? "[ OK ]" : "[FAIL]");
    // only a warning, nothing needs the memory size yet (the heap is a fixed size array)
    if (boot_multiboot_valid) {
        printf("[ OK ] Memory: %u KB lower, %u KB upper\n", sys_mem_lower_kb, sys_mem_upper_kb);
    } else {
        printf("[WARN] No memory information from the bootloader\n");
    }
    printf("[ OK ] Kernel heap: %u KB\n", (unsigned int)(heap_total() / 1024));

    // rtc_datetime() fills in now and returns it, so it can go straight into printf
    char now[FS_DATETIME_MAX];
    printf("[ OK ] Real time clock: %s\n\n", rtc_datetime(now, sizeof(now)));

    // initialise the file system (the disk, or the temporary one if there is no disk)
    // "n" = normal: mount a FAT disk labelled RENOVATE if there is one, otherwise use the VFS
    // "d" = demo: go straight to the VFS without probing the disks at all
    fs_init(boot_option == BOOT_OPT_DEMO ? "d" : "n");

    // beep once
    //beep(0);

    // the file system is up (on disk or in memory), the terminal can use it from here on
    printf("\n[VFS:UP]\n");

    // give the user time to read the log, unless fast boot was chosen
    if (boot_option != BOOT_OPT_FAST) {
        clck_hang(BOOT_LOG_DELAY);
    }

    return 0;
}

#include "boot.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/input/IO.h"
#include "../lib/ren_stdlib.h"
#include "../lib/ren_stdio.h"
#include "interrupts.h"
#include "sys_clck.h"
#include "f_sys.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002
#define MULTIBOOT_INFO_MEMORY      0x00000001
#define BOOT_LOG_DELAY             2000 // how long the boot log stays on screen (ms)

// the start of the information structure GRUB hands to the kernel
typedef struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower; // KB of memory below 1MB
    uint32_t mem_upper; // KB of memory above 1MB
} multiboot_info_t;

int boot_option = BOOT_OPT_NORMAL;
bool boot_options_requested = false;

bool boot_multiboot_valid = false;
uint32_t sys_mem_lower_kb = 0;
uint32_t sys_mem_upper_kb = 0;

// read what the bootloader told us about the machine
void boot_parse_multiboot (uint32_t magic, uint32_t info_addr) {
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC || info_addr == 0) {
        return;
    }
    const multiboot_info_t* info = (const multiboot_info_t*)(uintptr_t)info_addr;
    boot_multiboot_valid = true;
    if (info->flags & MULTIBOOT_INFO_MEMORY) {
        sys_mem_lower_kb = info->mem_lower;
        sys_mem_upper_kb = info->mem_upper;
    }
}

int init_boot (void) {
    console_set_color(BLACK, BRIGHT);
    console_clear();

    printf("[E:b1f1] Renovate OS boot sequence started\n\n");

    printf("%s Descriptor tables & interrupts (GDT / IDT / PIC)\n", interrupts_ready ? "[ OK ]" : "[FAIL]");
    printf("%s System timer at %u Hz\n", clck_ready ? "[ OK ]" : "[FAIL]", (unsigned int)PIT_HZ);
    printf("%s PS/2 keyboard\n", keyboard_ready ? "[ OK ]" : "[FAIL]");
    if (boot_multiboot_valid) {
        printf("[ OK ] Memory: %u KB lower, %u KB upper\n", sys_mem_lower_kb, sys_mem_upper_kb);
    } else {
        printf("[WARN] No memory information from the bootloader\n");
    }
    printf("[ OK ] Kernel heap: %u KB\n", (unsigned int)(heap_total() / 1024));

    char now[FS_DATETIME_MAX];
    printf("[ OK ] Real time clock: %s\n\n", rtc_datetime(now, sizeof(now)));

    // initialise the file system (the disk, or the temporary one if there is no disk)
    fs_init(boot_option == BOOT_OPT_DEMO ? "d" : "n");

    // beep once
    //beep(0);

    printf("\n[VFS:UP]\n");

    if (boot_option != BOOT_OPT_FAST) {
        clck_hang(BOOT_LOG_DELAY);
    }

    return 0;
}

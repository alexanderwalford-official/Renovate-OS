#ifndef BOOT_H
#define BOOT_H

#include <stdint.h>
#include <stdbool.h>

// boot options, chosen from the boot options menu (press X on the splash screen)
#define BOOT_OPT_NORMAL 0
#define BOOT_OPT_FAST   1 // skip the boot log delay
#define BOOT_OPT_DEMO   2 // VFS mode, the disks are never touched

extern int boot_option;
extern bool boot_options_requested;

// what the bootloader told us about the machine
extern bool boot_multiboot_valid;
extern uint32_t sys_mem_lower_kb; // KB of memory below 1MB
extern uint32_t sys_mem_upper_kb; // KB of memory above 1MB

void boot_parse_multiboot (uint32_t magic, uint32_t info_addr);
int init_boot (void);

#endif /* BOOT_H */

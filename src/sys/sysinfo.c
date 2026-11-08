/*
    sysinfo.c - what AerOS knows about the computer (see sysinfo.h)

    Most of it is already known to some other part of AerOS (the boot code, the drivers, the
    file system) and is just put into words here. The processor is the exception: it is asked
    directly with the CPUID instruction, which answers questions numbered by "leaf":

        leaf 0           the maker, as 12 letters ("GenuineIntel", "AuthenticAMD")
        leaf 1           family / model / stepping in EAX, feature bits in ECX and EDX,
                         and ECX bit 31 = "running under a hypervisor"
        leaf 0x40000000  the hypervisor's name, as 12 letters ("KVMKVMKVM", "TCGTCGTCGTCG"...)
        leaf 0x80000000  the highest "extended" leaf
        leaf 0x80000001  more feature bits, EDX bit 29 = 64 bit capable (x86-64)
        leaf 0x80000002 to 0x80000004  the full name, 48 characters ("Intel(R) Core(TM) i7...")

    https://en.wikipedia.org/wiki/CPUID
*/

#include "sysinfo.h"
#include <stdint.h>
#include "../lib/primary_definitions.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/video/bga.h"
#include "../drivers/input/IO.h"
#include "../drivers/input/GIO.h"
#include "../drivers/storage/ata.h"
#include "../drivers/audio/ac97.h"
#include "../drivers/pci/pci.h"
#include "accounts/auth.h"
#include "interrupts.h"
#include "sys_clck.h"
#include "f_sys.h"
#include "boot.h"

#define KERNEL_START 0x00100000 // where linker.ld puts the kernel
extern char sbss[]; // the end of the kernel file's contents (the zeroed .bss isn't in the file)
extern char end[];  // the end of the kernel in memory

typedef struct {
    uint32_t a, b, c, d;
} cpuid_t;

static cpuid_t cpuid (uint32_t leaf) {
    cpuid_t r;
    __asm__ volatile ("cpuid" : "=a"(r.a), "=b"(r.b), "=c"(r.c), "=d"(r.d) : "a"(leaf), "c"(0));
    return r;
}

static uint32_t read_tsc_low (void) {
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return low;
}

// helpers that format a value and hand the row on
typedef struct {
    sysinfo_fn fn;
    void* ctx;
    const char* group;
} out_t;

__attribute__((format(printf, 3, 4)))
static void row (out_t* out, const char* name, const char* format, ...) {
    char value[96];
    va_list ap;
    va_start(ap, format);
    vsnprintf(value, sizeof(value), format, ap);
    va_end(ap);
    out->fn(out->group, name, value, out->ctx);
}

// ---- AerOS itself -----------------------------------------------------------------------------

static void aeros_rows (out_t* out) {
    out->group = "AerOS";
    row(out, "Version", "%s", AEROS_VERSION);
    const char* mode = fs_mode == FS_MODE_DISK ? "DISK (files are saved to the AEROS disk)"
                     : boot_option == BOOT_OPT_DEMO ? "DEMO (nothing is saved)"
                     : "VFS (no AEROS disk, files are kept in memory)";
    row(out, "Mode", "%s", mode);
    row(out, "User", "%s", auth_user);
    uint32_t seconds = clck_uptime_ms() / 1000;
    row(out, "Running for", "%u:%02u:%02u", seconds / 3600, seconds / 60 % 60, seconds % 60);
    row(out, "Kernel", "%u KB file, %u KB in memory",
        (uint32_t)((uintptr_t)sbss - KERNEL_START) / 1024, (uint32_t)((uintptr_t)end - KERNEL_START) / 1024);
}

// ---- the processor ----------------------------------------------------------------------------

// 12 letters held in three registers, in the order the leaf uses
static void registers_text (uint32_t first, uint32_t second, uint32_t third, char out[13]) {
    memcpy(out, &first, 4);
    memcpy(out + 4, &second, 4);
    memcpy(out + 8, &third, 4);
    out[12] = '\0';
}

// the hypervisor's signature in plain words
static const char* hypervisor_name (const char* id) {
    static const struct { const char* id; const char* name; } known[] = {
        { "TCGTCGTCGTCG", "QEMU (an emulated processor)" }, { "KVMKVMKVM", "KVM" },
        { "VBoxVBoxVBox", "VirtualBox" }, { "VMwareVMware", "VMware" },
        { "Microsoft Hv", "Hyper-V" }, { "XenVMMXenVMM", "Xen" }, { " lrpepyh  vr", "Parallels" },
    };
    for (unsigned int i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strncmp(id, known[i].id, strlen(known[i].id)) == 0) return known[i].name;
    }
    return id;
}

static void processor_rows (out_t* out) {
    out->group = "Processor";
    cpuid_t r = cpuid(0);
    char vendor[13];
    registers_text(r.b, r.d, r.c, vendor); // the vendor is in EBX, EDX, ECX order
    uint32_t max_extended = cpuid(0x80000000).a;

    // the full name if the processor has one, otherwise just the maker
    if (max_extended >= 0x80000004) {
        char name[49];
        for (uint32_t i = 0; i < 3; i++) {
            cpuid_t part = cpuid(0x80000002 + i);
            memcpy(name + i * 16, &part, 16);
        }
        name[48] = '\0';
        row(out, "Name", "%s", StrTrim(name));
    }
    row(out, "Made by", "%s", vendor);

    cpuid_t info = cpuid(1);
    uint32_t family = (info.a >> 8) & 0xF, model = (info.a >> 4) & 0xF;
    if (family == 0xF) family += (info.a >> 20) & 0xFF;               // the extended family
    if (family == 6 || family >= 0xF) model += ((info.a >> 16) & 0xF) << 4; // and model
    row(out, "Family", "%u, model %u, stepping %u", family, model, info.a & 0xF);

    // features that matter to an OS, from the leaf 1 bits
    bool has_tsc = info.d & (1u << 4);
    bool is_64 = max_extended >= 0x80000001 && (cpuid(0x80000001).d & (1u << 29));
    row(out, "Features", "%s%s%s%s%s%s%s%s",
        (info.d & 1) ? "FPU " : "", has_tsc ? "TSC " : "", (info.d & (1u << 25)) ? "SSE " : "",
        (info.d & (1u << 26)) ? "SSE2 " : "", (info.c & (1u << 20)) ? "SSE4.2 " : "",
        (info.c & (1u << 28)) ? "AVX " : "", (info.c & (1u << 30)) ? "RDRAND " : "",
        is_64 ? "x86-64" : "");

    // the speed: count the processor's cycles over 20ms of the timer. Only the low 32 bits of
    // the counter are needed, 20ms is far fewer than 4 billion cycles even at 10 GHz
    if (has_tsc && clck_ready && interrupts_enabled()) {
        uint32_t before = read_tsc_low();
        sleep_ms(20);
        uint32_t cycles = read_tsc_low() - before;
        row(out, "Speed", "about %u MHz", cycles / 20000);
    }

    if (info.c & (1u << 31)) {
        cpuid_t h = cpuid(0x40000000);
        char id[13];
        registers_text(h.b, h.c, h.d, id);
        row(out, "Running in", "%s", hypervisor_name(id));
    } else {
        row(out, "Running in", "%s", "a real computer, as far as it can tell");
    }
}

// ---- memory -----------------------------------------------------------------------------------

static void memory_rows (out_t* out) {
    out->group = "Memory";
    if (boot_multiboot_valid && sys_mem_upper_kb > 0) {
        row(out, "RAM", "%u MB", (sys_mem_upper_kb + 1024) / 1024);
    } else {
        row(out, "RAM", "%s", "unknown (the bootloader didn't say)");
    }
    uint32_t used_kb = (uint32_t)(heap_used() / 1024), total_kb = (uint32_t)(heap_total() / 1024);
    row(out, "Heap", "%u KB used of %u MB (%u%%)", used_kb, total_kb / 1024,
        total_kb ? used_kb * 100 / total_kb : 0);
    row(out, "Stack", "%s", "256 KB, shared by everything");
}

// ---- screen and input -------------------------------------------------------------------------

static void screen_rows (out_t* out) {
    out->group = "Screen and input";
    vga_framebuffer_t fb;
    if (vga_framebuffer_info(&fb)) {
        row(out, "Screen", "%u x %u, %u bit colour", fb.width, fb.height, fb.bpp);
        row(out, "Framebuffer", "at 0x%x, %u bytes a line", fb.address, fb.pitch);
        row(out, "Graphics card", "%s", bga_available() ? "Bochs / QEMU / VirtualBox (RES changes the resolution)"
                                                         : "set up by the firmware (fixed resolution)");
    } else {
        row(out, "Screen", "%s", "VGA text mode");
    }
    row(out, "Console", "%u x %u characters", (unsigned int)COLS, (unsigned int)ROWS);
    row(out, "Keyboard", "%s", keyboard_ready ? "PS/2" : "none found");
}

// ---- storage ----------------------------------------------------------------------------------

// clusters -> MB. A cluster can be a single 512 byte sector, and clusters * sectors per cluster
// can pass 4 billion on a big disk, so it is worked out in whole MB first (2048 sectors) and the
// remainder after
static uint32_t clusters_to_mb (uint32_t clusters) {
    uint32_t per = fat_vol.sectors_per_cluster;
    return clusters / 2048 * per + clusters % 2048 * per / 2048;
}

static void storage_rows (out_t* out) {
    out->group = "Storage";
    static const char* const positions[4] = { "Primary master", "Primary slave", "Secondary master", "Secondary slave" };
    for (int i = 0; i < 4; i++) {
        int bus = i / 2, slave = i % 2;
        char model[41] = "";
        uint32_t sectors = 0;
        int type = drive_probe_info(bus, slave, model, sizeof(model), &sectors);
        if (type == DRIVE_NONE) continue;
        bool mounted = fs_mode == FS_MODE_DISK && fat_vol.bus == bus && fat_vol.slave == slave;
        char size[24] = "";
        if (sectors) snprintf(size, sizeof(size), ", %u MB", sectors / 2048);
        const char* name = StrTrim(model);
        row(out, positions[i], "%s%s%s", name[0] ? name : drive_type_name(type), size,
            mounted ? ", the AEROS disk" : "");
    }
    if (fs_mode == FS_MODE_DISK) {
        uint32_t total_mb = clusters_to_mb(fat_vol.cluster_count);
        uint32_t free_mb = clusters_to_mb(fat_free_clusters());
        uint32_t used_mb = total_mb > free_mb ? total_mb - free_mb : 0;
        row(out, "File system", "FAT%d, labelled %s", fat_vol.type, fat_vol.label);
        row(out, "Space", "%u MB used of %u MB (%u%%)", used_mb, total_mb, total_mb ? used_mb * 100 / total_mb : 0);
    } else {
        row(out, "File system", "%s", "in memory, gone when the computer turns off");
    }
}

// ---- other hardware ---------------------------------------------------------------------------

static void hardware_rows (out_t* out) {
    out->group = "Other hardware";
    row(out, "Sound", "%s", ac97_ready ? ac97_name() : "no AC'97 card found");
    row(out, "Beeper", "%s", "PC speaker");
    row(out, "Timer", "%s", clck_ready ? "PIT, 1000 ticks a second" : "PIT not ticking, using busy waits");
    row(out, "Interrupts", "%s", interrupts_ready ? "set up (PIC remapped to 32-47)" : "not set up");
    char now[FS_DATETIME_MAX];
    row(out, "Clock", "%s", rtc_datetime(now, sizeof(now)));
}

// ---- PCI devices ------------------------------------------------------------------------------

static const char* vendor_name (uint16_t vendor) {
    switch (vendor) {
        case 0x8086: return "Intel";
        case 0x1022: return "AMD";
        case 0x1002: return "AMD / ATI";
        case 0x10DE: return "NVIDIA";
        case 0x10EC: return "Realtek";
        case 0x1234: return "QEMU";
        case 0x1AF4: return "Red Hat (virtio)";
        case 0x80EE: return "VirtualBox";
        case 0x15AD: return "VMware";
        case 0x1B36: return "Red Hat (QEMU)";
        default: return "";
    }
}

// what kind of device a class and subclass are (https://wiki.osdev.org/PCI#Class_Codes)
static const char* class_name (uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
        case 0x01:
            switch (subclass) {
                case 0x01: return "IDE disk controller";
                case 0x06: return "SATA disk controller";
                case 0x08: return "NVMe disk controller";
                default: return "disk controller";
            }
        case 0x02: return "network card";
        case 0x03: return subclass == 0x00 ? "VGA graphics card" : "graphics card";
        case 0x04:
            switch (subclass) {
                case 0x01: return "AC'97 sound card";
                case 0x03: return "HD Audio sound card";
                default: return "multimedia device";
            }
        case 0x05: return "memory controller";
        case 0x06:
            switch (subclass) {
                case 0x00: return "host bridge";
                case 0x01: return "ISA bridge";
                case 0x04: return "PCI bridge";
                default: return "bridge";
            }
        case 0x07: return "communication controller";
        case 0x08: return "system device";
        case 0x09: return "input device";
        case 0x0C:
            switch (subclass) {
                case 0x03: return "USB controller";
                case 0x05: return "SMBus controller";
                default: return "serial bus controller";
            }
        default: return "other device";
    }
}

static bool pci_row (const pci_device_t* dev, void* ctx) {
    out_t* out = ctx;
    char where[12];
    snprintf(where, sizeof(where), "%02x:%02x.%u", dev->bus, dev->slot, dev->function);
    const char* vendor = vendor_name(dev->vendor);
    row(out, where, "%s%s%s (%04x:%04x)", vendor, vendor[0] ? " " : "",
        class_name(dev->class_code, dev->subclass), dev->vendor, dev->device);
    return false; // keep going
}

void sysinfo_collect (sysinfo_fn fn, void* ctx) {
    out_t out = { fn, ctx, "" };
    aeros_rows(&out);
    processor_rows(&out);
    memory_rows(&out);
    screen_rows(&out);
    storage_rows(&out);
    hardware_rows(&out);
    out.group = "PCI devices";
    pci_each(pci_row, &out);
}

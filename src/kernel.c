/*
    AerOS
    Developed by Alexander Walford for Renovate Software LTD 2023

    X86 32 bit protected mode, loaded by GRUB (multiboot)

    kernel.c - where the operating system begins.

    How we get here:
    1. The BIOS (or UEFI in legacy mode) loads GRUB from the ISO.
    2. GRUB reads iso/boot/grub/grub.cfg, finds main.elf and spots our multiboot header
       (the magic number at the top of entry.asm), loads the kernel to 1MB, switches the CPU
       into 32 bit protected mode and jumps to "loader" in entry.asm.
    3. entry.asm turns interrupts off, sets up a 32KB stack, pushes the two values GRUB left
       in the registers and calls main() below.

    What main() does, in order:
    - Hardware setup: the screen, the serial port, reading the memory map from GRUB, our own
      descriptor tables (GDT / IDT), the timer and the keyboard. Only once every interrupt
      has somewhere to go do we switch interrupts on.
    - Startup processes: everything after that runs as a "process" through the process manager
      (sys/p_man.c). A process is just a line of RenLang code (e.g. "splash_screen") that the
      compiler (lib/ren_comp.c) turns into a function call. The boot goes:
          splash_screen -> (boot options, if X was pressed) -> init_boot -> terminal
    - The terminal never returns in practice. Reboot and shut down are done from inside it
      (see sys/power.c), so the last few lines of main() are a safety net.

    Useful reading:
    https://wiki.osdev.org/Bare_Bones
    https://wiki.osdev.org/Multiboot
    https://www.gnu.org/software/grub/manual/multiboot/multiboot.html
*/

#include <stdint.h>
#include "drivers/input/GIO.h"
#include "drivers/input/IO.h"
#include "drivers/serial/serial.h"
#include "drivers/video/VGA_linear.h"
#include "sys/interrupts.h"
#include "sys/sys_clck.h"
#include "sys/p_man.h"
#include "sys/boot.h"
#include "sys/recovery.h"

// called from entry.asm with the multiboot information address and magic number GRUB left in ebx / eax
// mb_info_addr = physical address of GRUB's multiboot information structure (see sys/boot.c)
// mb_magic     = 0x2BADB002 if a multiboot compliant bootloader started us, anything else means it didn't
// entry.asm pushes eax first and ebx second. C (cdecl) expects the first argument to be the value
// pushed last, which is why the address comes out as the first argument and the magic as the second.
// returns 0, but only if the terminal ever finishes (entry.asm then halts the CPU forever)
int main(uint32_t mb_info_addr, uint32_t mb_magic) {

    // hardware setup, this has to happen before any process runs
    // the order matters:
    // - the multiboot info is read first, before anything has a chance to overwrite the memory it sits in,
    //   and because it says whether the screen is VGA text mode or a framebuffer (UEFI)
    // - the screen comes next so that anything that goes wrong later can at least be seen
    // - the GDT has to be ours before the IDT points at code segments in it (GRUB's GDT may not stay valid)
    // - the timer and keyboard install IRQ handlers, so they need the IDT and the remapped PIC first
    // - interrupts are only switched on at the very end. If an IRQ arrived before the IDT was ready the CPU
    //   would have nowhere to jump, fault, fail to handle that too and reset (a "triple fault")
    // every process below depends on this: the splash screen times itself with the timer and listens for
    // the keyboard, both of which only work once IRQ0 / IRQ1 are being delivered
    boot_parse_multiboot(mb_magic, mb_info_addr); // memory size and the screen GRUB set up
    vga_init(boot_framebuffer_valid ? &boot_framebuffer : 0); // screen & cursor
    serial_init();                                // debug output on COM1
    gdt_init();                                   // segments
    idt_init();                                   // exception & IRQ handlers, PIC remap
    clck_init();                                  // 1000Hz system timer (IRQ0)
    init_keyboard();                              // PS/2 keyboard (IRQ1)
    interrupts_enable();                          // "sti", from here on the timer and keyboard are live
    clck_verify();                                // fall back to busy waits if the PIT is switched off

    // each startup step is its own process: create it (it waits as DEAD), execute it, then destroy it
    // so its slot on the process stack is free again. Because the slot is freed every time, each of
    // these processes ends up re-using process ID 0.
    int pid;

    // processes for startup
    // the splash screen shows the logo for 4 seconds and listens for the X key
    pid = p_create("splash_screen");
    p_exec(pid);
    p_destroy(pid);

    // if X was pressed on the splash screen, open the boot options
    // (the menu sets boot_option to normal / fast / demo, or reboots / shuts down straight away)
    if (boot_options_requested) {
        init_recovery();
    }

    // the boot log: prints what was set up above, mounts the file system and waits a moment
    // (boot_option decides whether the disks are used and whether the wait is skipped)
    pid = p_create("init_boot");
    p_exec(pid);
    p_destroy(pid);

    // the terminal runs until the computer is turned off
    // while it runs, the "terminal" process stays RUNNING on the process stack, which is how
    // a nested p_exec (e.g. the RUN command) knows not to start it a second time
    pid = p_create("terminal");
    p_exec(pid);
    p_destroy(pid);

    // only reached if the terminal ever stops, entry.asm then disables interrupts and halts
    return 0;
}

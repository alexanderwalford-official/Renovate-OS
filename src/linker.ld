/*
    linker.ld
    Tells the linker (ld) how to glue every object file together into main.elf: where in memory
    the kernel lives, which sections go first and in what order, and what to throw away.

    The resulting layout in memory (physical addresses, there is no paging yet):

      0x00100000  .mbheader   multiboot header (12 bytes), first so GRUB can find it
                  .text       all the code, starting with entry.asm's loader
      next 4KB    .rodata     constants and string literals
      next 4KB    .data       initialised global variables
                  .bss        zeroed globals: the 256KB stack, the 4MB heap pool, the IDT...
                  end         first free byte after the kernel

    Why 1MB? Everything below it is the old real mode world: the BIOS interrupt table and data
    area, the EBDA, VGA memory at 0xA0000-0xBFFFF, option ROMs and the BIOS itself at
    0xF0000-0xFFFFF. 1MB is the first address with a big block of ordinary RAM, and it is where
    multiboot kernels traditionally go.

    The Makefile links with --gc-sections and compiles with -ffunction-sections / -fdata-sections,
    so every function and variable gets its own section and any section nothing refers to is
    thrown away. That keeps the kernel small, but it is also why some things need KEEP().

    https://wiki.osdev.org/Linker_Scripts
    https://sourceware.org/binutils/docs/ld/Scripts.html
*/

/* the ELF entry point, GRUB jumps here once the kernel is loaded (see entry.asm) */
ENTRY (loader)
SECTIONS
{
  /* "." is the location counter: start placing everything at 1MB */
  . = 0x00100000;
  /* the multiboot header must be within the first 8KB of the kernel */
  /* putting its section first guarantees that. KEEP() is essential: no code ever refers to the
     header (only GRUB reads it), so --gc-sections would otherwise delete it and GRUB would
     refuse to boot with "no multiboot header found" */
  .mbheader : {
    KEEP(*(.mbheader))
  }
  /* GCC also emits .text.startup, .rodata.str1.1 etc., so match every sub-section */
  /* the code. With -ffunction-sections each function is in its own .text.<name> section,
     and GCC puts main() in .text.startup.main */
  .text : {
    *(.text .text.*)
  }
  /* read only data (constants, string literals, lookup tables), started on a fresh 4KB page.
     Once paging exists, code, constants and writable data can then be given different
     permissions without sharing a page */
  .rodata ALIGN (0x1000) : {
    *(.rodata .rodata.*)
  }
  /* global and static variables with a starting value, also page aligned */
  .data ALIGN (0x1000) : {
    *(.data .data.*)
  }
  /* variables that start at zero. They take no space in main.elf, GRUB zeroes this memory while
     loading. sbss / ebss mark its start and end (sys/sysinfo.c uses sbss to work out how big
     the kernel file is). COMMON holds old style uninitialised globals, which modern GCC
     no longer creates by default, but it does no harm to collect them */
  .bss : {
    sbss = .;
    *(COMMON)
    *(.bss .bss.*)
    ebss = .;
  }
  /* the first byte after the whole kernel image, the traditional name for "free memory starts
     here" (the boot code only gives the heap memory above it, and sysinfo reports it) */
  end = .;

  /* sections the kernel does not need: .comment holds the compiler version string, .eh_frame
     is unwind information for C++ exceptions and debuggers, and .note.* are notes such as
     .note.GNU-stack and the build ID */
  /DISCARD/ : {
    *(.comment)
    *(.eh_frame)
    *(.note .note.*)
  }
}

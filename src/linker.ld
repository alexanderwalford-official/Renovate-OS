ENTRY (loader)
SECTIONS
{
  . = 0x00100000;
  /* the multiboot header must be within the first 8KB of the kernel */
  .mbheader : {
    KEEP(*(.mbheader))
  }
  /* GCC also emits .text.startup, .rodata.str1.1 etc., so match every sub-section */
  .text : {
    *(.text .text.*)
  }
  .rodata ALIGN (0x1000) : {
    *(.rodata .rodata.*)
  }
  .data ALIGN (0x1000) : {
    *(.data .data.*)
  }
  .bss : {
    sbss = .;
    *(COMMON)
    *(.bss .bss.*)
    ebss = .;
  }
  end = .;

  /DISCARD/ : {
    *(.comment)
    *(.eh_frame)
    *(.note .note.*)
  }
}

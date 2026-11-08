; ============================================================================================
; entry.asm
; The very first Renovate OS code that runs: the multiboot header and the jump into C.
;
; Multiboot is a standard contract between a bootloader (GRUB) and a kernel. GRUB looks for a
; small "multiboot header" in the first 8KB of the kernel file, checks it, loads the kernel's
; ELF segments where linker.ld says they belong (1MB) and jumps to the ELF entry point, which is
; "loader" below. When we arrive here the machine is in this state:
;   - 32 bit protected mode, paging off, A20 line enabled (all 4GB addressable)
;   - CS / DS / ES / FS / GS / SS hold flat 4GB segments, but the GDT that describes them is
;     GRUB's and may be overwritten later, so gdt_init() builds our own
;   - interrupts disabled, and there is no IDT that we can use
;   - eax = 0x2BADB002, the magic number that proves a multiboot loader started us
;   - ebx = physical address of the multiboot information structure (memory size, memory map,
;     command line...), read by boot_parse_multiboot() in sys/boot.c
;   - esp is undefined! The kernel must set up its own stack before calling any C code
;
; The job of this file is just: set up a stack, hand eax / ebx to main() in kernel.c, and park
; the CPU if main() ever returns.
;
; https://wiki.osdev.org/Multiboot
; https://www.gnu.org/software/grub/manual/multiboot/multiboot.html
; https://wiki.osdev.org/Bare_Bones
; ============================================================================================

; loader is exported so linker.ld's ENTRY(loader) can find it, stack_ptr marks the top of the stack
global loader
global stack_ptr

; main() lives in kernel.c
extern main

; multiboot header flags, each bit asks GRUB for something:
;   bit 0  load any boot modules on 4KB page boundaries
;   bit 1  fill in the memory fields (mem_lower / mem_upper and the memory map) of the
;          multiboot information structure, so the kernel knows how much RAM there is
; bit 16 (the "a.out kludge", load addresses in the header) is not needed because GRUB reads
; them from our ELF file instead
MODULEALIGN equ 1<<0
MEMINFO equ 1<<1
FLAGS equ MODULEALIGN | MEMINFO
; the magic number GRUB searches the file for (it hands back a different one, 0x2BADB002, in eax)
MAGIC equ 0x1BADB002
; MAGIC + FLAGS + CHECKSUM must add up to 0 (in 32 bits), that is how GRUB knows it found a
; real header and not three random numbers that happen to start with the magic value
CHECKSUM equ -(MAGIC + FLAGS)

; the header gets its own section so linker.ld can put it at the very start of the kernel
; (it has to be within the first 8KB and 4 byte aligned)
section .mbheader
align 4
MultiBootHeader:
  dd MAGIC
  dd FLAGS
  dd CHECKSUM

section .text

; 0x8000 = 32KB of kernel stack, shared by main(), every process and every interrupt handler
; (there is no separate interrupt stack). Recursion or large local arrays eat into it
STACKSIZE equ 0x8000

loader:
  cli                       ; no interrupts until the kernel has set up its IDT
  cld                       ; C code expects the direction flag to be clear
  ; the x86 stack grows downwards, so esp starts at the TOP (highest address) of the stack area
  mov esp, stack+STACKSIZE
  ; cdecl pushes arguments right to left, so the last value pushed becomes the first argument:
  ; this calls main(mb_info_addr = ebx, mb_magic = eax). Nothing above has touched eax or ebx
  push eax                  ; main(arg 2) = multiboot magic number
  push ebx                  ; main(arg 1) = address of the multiboot information

  call main

  ; main() should run the terminal forever, but if it ever returns there is nowhere to go back
  ; to, so switch off interrupts and halt for good
  cli

hang:
  hlt
  ; an NMI could still wake the CPU from hlt, so loop straight back to sleep
  jmp hang

; .bss is memory that starts zeroed and takes no space in the kernel file: the ELF file just
; says "reserve this much", and GRUB clears it while loading. Perfect for a big stack
section .bss
; 16 byte alignment keeps the stack nicely aligned for the compiler's code
align 16
stack:
  ; reserve STACKSIZE bytes (resb = reserve bytes, no data is written)
  resb STACKSIZE
; the end of the reserved area = the top of the stack, where esp starts
stack_ptr:

; mark the stack as non-executable (silences the linker warning)
section .note.GNU-stack noalloc noexec nowrite progbits

global loader
global stack_ptr

extern main

MODULEALIGN equ 1<<0
MEMINFO equ 1<<1
FLAGS equ MODULEALIGN | MEMINFO
MAGIC equ 0x1BADB002
CHECKSUM equ -(MAGIC + FLAGS)

section .mbheader
align 4
MultiBootHeader:
  dd MAGIC
  dd FLAGS
  dd CHECKSUM

section .text

STACKSIZE equ 0x8000

loader:
  cli                       ; no interrupts until the kernel has set up its IDT
  cld                       ; C code expects the direction flag to be clear
  mov esp, stack+STACKSIZE
  push eax                  ; main(arg 2) = multiboot magic number
  push ebx                  ; main(arg 1) = address of the multiboot information

  call main

  cli

hang:
  hlt
  jmp hang

section .bss
align 16
stack:
  resb STACKSIZE
stack_ptr:

; mark the stack as non-executable (silences the linker warning)
section .note.GNU-stack noalloc noexec nowrite progbits

/*
    interrupts.h
    CPU descriptor tables and hardware interrupt handling for 32 bit protected mode.

    GRUB starts the kernel in protected mode, where the real mode BIOS interrupt table (IVT)
    no longer works. Before interrupts can be enabled ("sti") the kernel needs:
      - its own GDT (the multiboot spec does not guarantee GRUB's one stays valid)
      - an IDT with a handler for every CPU exception and hardware IRQ
      - the PIC remapped so IRQs 0-15 do not collide with CPU exceptions 0-31

    https://wiki.osdev.org/GDT_Tutorial
    https://wiki.osdev.org/Interrupt_Descriptor_Table
    https://wiki.osdev.org/8259_PIC
*/
#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>
#include <stdbool.h>

#define KERNEL_CODE_SELECTOR 0x08
#define KERNEL_DATA_SELECTOR 0x10

// CPU state pushed by the interrupt stubs, in the order it sits on the stack
struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; // pushed by pusha
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;                        // pushed by the CPU
};

typedef void (*irq_handler_t)(struct regs* r);

extern bool interrupts_ready;

void gdt_init(void);
void idt_init(void);

void pic_unmask(uint8_t irq);
void pic_mask(uint8_t irq);

void irq_install_handler(uint8_t irq, irq_handler_t handler);
void irq_uninstall_handler(uint8_t irq);

#endif /* INTERRUPTS_H */

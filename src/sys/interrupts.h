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
#include "../drivers/input/GIO.h"
#include "../drivers/video/VGA_linear.h"
#include "../lib/primary_definitions.h"

#define KERNEL_CODE_SELECTOR 0x08
#define KERNEL_DATA_SELECTOR 0x10

/* ------------------------------------------------------------------ */
/*  GDT                                                                 */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} gdt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} gdt_ptr_t;

gdt_entry_t gdt[3];
gdt_ptr_t gdt_ptr;

void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity) {
    gdt[index].base_low = (uint16_t)(base & 0xFFFF);
    gdt[index].base_mid = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].base_high = (uint8_t)((base >> 24) & 0xFF);
    gdt[index].limit_low = (uint16_t)(limit & 0xFFFF);
    gdt[index].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (granularity & 0xF0));
    gdt[index].access = access;
}

// flat memory model: one code and one data segment covering all 4GB
void gdt_init(void) {
    gdt_set_entry(0, 0, 0, 0, 0);                // null descriptor
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // kernel code (0x08)
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // kernel data (0x10)

    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint32_t)(uintptr_t)&gdt;

    __asm__ volatile (
        "lgdt %0\n\t"
        "ljmp $0x08, $1f\n"     // far jump reloads CS
        "1:\n\t"
        "movw $0x10, %%ax\n\t"  // reload every data segment register
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        : : "m"(gdt_ptr) : "eax", "memory");
}

/* ------------------------------------------------------------------ */
/*  PIC (8259)                                                          */
/* ------------------------------------------------------------------ */

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20

#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01
#define ICW4_8086    0x01

#define IRQ_BASE_VECTOR 0x20 // IRQ0 -> interrupt 32

// move IRQ 0-7 to interrupts 0x20-0x27 and IRQ 8-15 to 0x28-0x2F, all IRQs start masked
void pic_remap(void) {
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC1_DATA, IRQ_BASE_VECTOR); io_wait();     // master PIC vector offset
    outb(PIC2_DATA, IRQ_BASE_VECTOR + 8); io_wait(); // slave PIC vector offset
    outb(PIC1_DATA, 4); io_wait();                   // slave PIC is on IRQ2
    outb(PIC2_DATA, 2); io_wait();                   // slave PIC cascade identity
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    outb(PIC1_DATA, 0xFB); // mask everything except IRQ2 (the cascade to the slave)
    outb(PIC2_DATA, 0xFF);
}

// allow an IRQ line to fire
void pic_unmask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(1 << (irq & 7));
    outb(port, inb(port) & (uint8_t)~bit);
}

// block an IRQ line
void pic_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(1 << (irq & 7));
    outb(port, inb(port) | bit);
}

// read the "in service" register of both PICs (used to detect spurious IRQs)
uint16_t pic_read_isr(void) {
    outb(PIC1_COMMAND, 0x0B);
    outb(PIC2_COMMAND, 0x0B);
    return (uint16_t)((inb(PIC2_COMMAND) << 8) | inb(PIC1_COMMAND));
}

/* ------------------------------------------------------------------ */
/*  IDT                                                                 */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint16_t offset_low;  // handler address bits 0-15
    uint16_t selector;    // code segment selector
    uint8_t  zero;        // always 0
    uint8_t  flags;       // type and attributes, 0x8E = present, ring 0, 32 bit interrupt gate
    uint16_t offset_high; // handler address bits 16-31
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} idt_ptr_t;

idt_entry_t idt[256];
idt_ptr_t idt_ptr;

// CPU state pushed by the interrupt stubs, in the order it sits on the stack
struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; // pushed by pusha
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;                        // pushed by the CPU
};

typedef void (*irq_handler_t)(struct regs* r);

irq_handler_t irq_handlers[16];
bool interrupts_ready = false;

void idt_set_gate(uint8_t vector, uint32_t handler, uint16_t selector, uint8_t flags) {
    idt[vector].offset_low = (uint16_t)(handler & 0xFFFF);
    idt[vector].offset_high = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[vector].selector = selector;
    idt[vector].zero = 0;
    idt[vector].flags = flags;
}

void irq_install_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

void irq_uninstall_handler(uint8_t irq) {
    if (irq < 16) {
        irq_handlers[irq] = 0;
    }
}

/*
    Interrupt entry stubs (inline assembly).
    Every stub pushes a dummy error code (if the CPU did not push one) and the interrupt number,
    then jumps to a common stub that saves all registers and calls isr_dispatch() in C.
*/
#define ISR_NOERR(n) \
    __asm__(".pushsection .text\n" \
            ".global isr" #n "\n" \
            "isr" #n ":\n\t" \
            "pushl $0\n\t" \
            "pushl $" #n "\n\t" \
            "jmp isr_common_stub\n" \
            ".popsection\n"); \
    extern void isr##n(void)

#define ISR_ERR(n) \
    __asm__(".pushsection .text\n" \
            ".global isr" #n "\n" \
            "isr" #n ":\n\t" \
            "pushl $" #n "\n\t" \
            "jmp isr_common_stub\n" \
            ".popsection\n"); \
    extern void isr##n(void)

#define IRQ_STUB(n, vector) \
    __asm__(".pushsection .text\n" \
            ".global irq" #n "\n" \
            "irq" #n ":\n\t" \
            "pushl $0\n\t" \
            "pushl $" #vector "\n\t" \
            "jmp isr_common_stub\n" \
            ".popsection\n"); \
    extern void irq##n(void)

__asm__(".pushsection .text\n"
        "isr_common_stub:\n\t"
        "pusha\n\t"
        "pushl %ds\n\t"
        "pushl %es\n\t"
        "pushl %fs\n\t"
        "pushl %gs\n\t"
        "movw $0x10, %ax\n\t"
        "movw %ax, %ds\n\t"
        "movw %ax, %es\n\t"
        "movw %ax, %fs\n\t"
        "movw %ax, %gs\n\t"
        "cld\n\t"
        "pushl %esp\n\t"          // argument: pointer to struct regs
        "call isr_dispatch\n\t"
        "addl $4, %esp\n\t"
        "popl %gs\n\t"
        "popl %fs\n\t"
        "popl %es\n\t"
        "popl %ds\n\t"
        "popa\n\t"
        "addl $8, %esp\n\t"       // remove interrupt number and error code
        "iret\n"
        ".popsection\n");

// CPU exceptions (only 8, 10-14, 17, 21, 29 and 30 push an error code)
ISR_NOERR(0);  ISR_NOERR(1);  ISR_NOERR(2);  ISR_NOERR(3);
ISR_NOERR(4);  ISR_NOERR(5);  ISR_NOERR(6);  ISR_NOERR(7);
ISR_ERR(8);    ISR_NOERR(9);  ISR_ERR(10);   ISR_ERR(11);
ISR_ERR(12);   ISR_ERR(13);   ISR_ERR(14);   ISR_NOERR(15);
ISR_NOERR(16); ISR_ERR(17);   ISR_NOERR(18); ISR_NOERR(19);
ISR_NOERR(20); ISR_ERR(21);   ISR_NOERR(22); ISR_NOERR(23);
ISR_NOERR(24); ISR_NOERR(25); ISR_NOERR(26); ISR_NOERR(27);
ISR_NOERR(28); ISR_ERR(29);   ISR_ERR(30);   ISR_NOERR(31);

// hardware IRQs (after remapping)
IRQ_STUB(0, 32);  IRQ_STUB(1, 33);  IRQ_STUB(2, 34);  IRQ_STUB(3, 35);
IRQ_STUB(4, 36);  IRQ_STUB(5, 37);  IRQ_STUB(6, 38);  IRQ_STUB(7, 39);
IRQ_STUB(8, 40);  IRQ_STUB(9, 41);  IRQ_STUB(10, 42); IRQ_STUB(11, 43);
IRQ_STUB(12, 44); IRQ_STUB(13, 45); IRQ_STUB(14, 46); IRQ_STUB(15, 47);

const char* exception_names[32] = {
    "DIVISION BY ZERO", "DEBUG", "NON MASKABLE INTERRUPT", "BREAKPOINT",
    "OVERFLOW", "BOUND RANGE EXCEEDED", "INVALID OPCODE", "DEVICE NOT AVAILABLE",
    "DOUBLE FAULT", "COPROCESSOR SEGMENT OVERRUN", "INVALID TSS", "SEGMENT NOT PRESENT",
    "STACK SEGMENT FAULT", "GENERAL PROTECTION FAULT", "PAGE FAULT", "RESERVED",
    "X87 FLOATING POINT", "ALIGNMENT CHECK", "MACHINE CHECK", "SIMD FLOATING POINT",
    "VIRTUALIZATION", "CONTROL PROTECTION", "RESERVED", "RESERVED",
    "RESERVED", "RESERVED", "RESERVED", "RESERVED",
    "HYPERVISOR INJECTION", "VMM COMMUNICATION", "SECURITY", "RESERVED"
};

// a CPU exception cannot be recovered from yet, show what happened and stop the machine
void exception_panic(struct regs* r) {
    char line[80];

    clear(RED);
    puts(5, 7, BLACK, RED, "==================================");
    puts(5, 9, BLACK, RED, ">> SYSTEM EXCEPTION <<");
    puts(5, 10, BLACK, RED, "The system encountered an error and was forced to stop.");
    puts(5, 11, BLACK, RED, "Message is as followed:");
    snprintf(line, sizeof(line), "[ ! ] CPU EXCEPTION %u >> %s", r->int_no, exception_names[r->int_no & 31]);
    puts(5, 12, BLACK, RED, line);
    snprintf(line, sizeof(line), "EIP=%08x  CS=%04x  EFLAGS=%08x  ERR=%08x", r->eip, r->cs, r->eflags, r->err_code);
    puts(5, 14, BLACK, RED, line);
    snprintf(line, sizeof(line), "EAX=%08x  EBX=%08x  ECX=%08x  EDX=%08x", r->eax, r->ebx, r->ecx, r->edx);
    puts(5, 15, BLACK, RED, line);
    if (r->int_no == 14) {
        uint32_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        snprintf(line, sizeof(line), "FAULTING ADDRESS (CR2)=%08x", cr2);
        puts(5, 16, BLACK, RED, line);
    }
    puts(5, 18, BLACK, RED, "The system has been halted, please restart your computer.");

    serial_write("\n*** CPU EXCEPTION: ");
    serial_write(exception_names[r->int_no & 31]);
    serial_write(" ***\n");

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

// called by isr_common_stub for every interrupt
__attribute__((used)) void isr_dispatch(struct regs* r) {
    if (r->int_no < 32) {
        exception_panic(r);
        return;
    }

    if (r->int_no < IRQ_BASE_VECTOR + 16) {
        uint8_t irq = (uint8_t)(r->int_no - IRQ_BASE_VECTOR);

        // spurious IRQs (electrical noise) must not be acknowledged
        if (irq == 7 && !(pic_read_isr() & (1 << 7))) {
            return;
        }
        if (irq == 15 && !(pic_read_isr() & (1 << 15))) {
            outb(PIC1_COMMAND, PIC_EOI); // the master still saw the cascade IRQ
            return;
        }

        if (irq_handlers[irq]) {
            irq_handlers[irq](r);
        }

        // end of interrupt
        if (irq >= 8) {
            outb(PIC2_COMMAND, PIC_EOI);
        }
        outb(PIC1_COMMAND, PIC_EOI);
    }
}

// fill the IDT, remap the PIC and load the IDT (interrupts stay disabled until interrupts_enable())
void idt_init(void) {
    void (*exception_stubs[32])(void) = {
        isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
        isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    };
    void (*irq_stubs[16])(void) = {
        irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7,
        irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
    };

    memset(idt, 0, sizeof(idt));
    for (int i = 0; i < 32; i++) {
        idt_set_gate((uint8_t)i, (uint32_t)(uintptr_t)exception_stubs[i], KERNEL_CODE_SELECTOR, 0x8E);
    }
    for (int i = 0; i < 16; i++) {
        idt_set_gate((uint8_t)(IRQ_BASE_VECTOR + i), (uint32_t)(uintptr_t)irq_stubs[i], KERNEL_CODE_SELECTOR, 0x8E);
    }

    pic_remap();

    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint32_t)(uintptr_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));

    interrupts_ready = true;
}

#endif /* INTERRUPTS_H */

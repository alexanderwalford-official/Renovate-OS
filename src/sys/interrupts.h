/*
    interrupts.h
    CPU descriptor tables and hardware interrupt handling for 32 bit protected mode.

    GRUB starts the kernel in protected mode, where the real mode BIOS interrupt table (IVT)
    no longer works. Before interrupts can be enabled ("sti") the kernel needs:
      - its own GDT (the multiboot spec does not guarantee GRUB's one stays valid)
      - an IDT with a handler for every CPU exception and hardware IRQ
      - the PIC remapped so IRQs 0-15 do not collide with CPU exceptions 0-31

    The three pieces in a bit more detail:

      GDT (Global Descriptor Table)
        In protected mode a segment register (CS, DS, SS...) no longer holds an address, it holds a
        "selector": an index into the GDT. Each GDT entry ("descriptor") describes a segment: where
        it starts (base), how big it is (limit), whether it is code or data and which privilege
        ring may use it. We use the "flat" model, where every segment starts at 0 and covers the
        whole 4GB address space, so segmentation effectively disappears and a pointer is simply a
        physical address (there is no paging yet either).

      IDT (Interrupt Descriptor Table)
        256 entries ("gates"), one per interrupt vector. When interrupt N happens the CPU looks up
        entry N, saves EFLAGS, CS and EIP on the stack and jumps to the address in the gate.
          vectors 0-31   CPU exceptions (divide by zero, general protection fault, page fault...)
                         these are reserved by Intel
          vectors 32-47  the 16 hardware IRQ lines, once the PIC has been remapped (see below)
          the rest       unused for now (left "not present")

      PIC (8259 Programmable Interrupt Controller)
        Two chained 8259 chips collect the interrupt lines from the hardware (timer, keyboard,
        disks...) and forward them to the CPU as an interrupt vector. The BIOS sets the first chip
        to vectors 8-15, which collides with the CPU exceptions (IRQ0, the timer, would look exactly
        like a double fault!), so we move them out of the way to 32-47.

    How an interrupt travels through Renovate OS:
      device raises its IRQ line -> PIC -> CPU looks up the IDT gate -> tiny assembly stub for
      that vector (interrupts.c) -> isr_common_stub saves every register as a "struct regs" ->
      isr_dispatch() in C -> either the exception panic screen, or the driver's irq_handler_t
      -> end of interrupt (EOI) sent to the PIC -> registers restored -> "iret" back to whatever
      the CPU was doing before.

    Start up order (see kernel.c main()):
      gdt_init() -> idt_init() -> drivers install handlers and unmask their IRQ (clck_init(),
      init_keyboard()) -> interrupts_enable() ("sti"). No IRQ can fire before "sti" because
      entry.asm cleared the interrupt flag (CPU exceptions can, which is why the IDT comes first).

    https://wiki.osdev.org/GDT_Tutorial
    https://wiki.osdev.org/Interrupt_Descriptor_Table
    https://wiki.osdev.org/8259_PIC
    https://wiki.osdev.org/Interrupt_Service_Routines
    https://wiki.osdev.org/Exceptions
    Intel 64 and IA-32 Software Developer's Manual, volume 3, chapters 3 (segments) and 6 (interrupts)
*/
#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>
#include <stdbool.h>

// segment selectors for the two GDT entries gdt_init() creates. A selector is
// (index * 8) | table bit | privilege bits: 0x08 = entry 1 (code), 0x10 = entry 2 (data),
// both in the GDT (bit 2 = 0) and ring 0 (bits 0-1 = 0), i.e. full kernel privilege
#define KERNEL_CODE_SELECTOR 0x08
#define KERNEL_DATA_SELECTOR 0x10

// CPU state pushed by the interrupt stubs, in the order it sits on the stack
//
// the stack grows downwards, so the LAST thing pushed ends up at the LOWEST address, which is
// the FIRST field of the struct. Reading the fields bottom to top gives the order things were
// pushed in:
//   1. the CPU pushes eflags, cs and eip (and an error code for some exceptions) as it enters
//      the gate. Because we are already in ring 0 there is no privilege change, so the CPU does
//      NOT push ss / esp, which is why there are no user_esp / ss fields here (yet)
//   2. the stub pushes a dummy error code of 0 if the CPU did not push one, then the vector number
//   3. isr_common_stub runs "pusha" (all 8 general purpose registers) and pushes the 4 data
//      segment registers
// isr_dispatch() gets a pointer to the gs field. Any field a handler changes is written back
// into the CPU when the stub returns, which is handy later on (e.g. returning a value in eax)
struct regs {
    // data segment registers, pushed last by isr_common_stub. pushl of a segment register
    // reserves 4 bytes but only the low 16 bits are meaningful
    uint32_t gs, fs, es, ds;
    // the "esp" here is the value from before pusha ran (it points at int_no), popa ignores it
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; // pushed by pusha
    // int_no = vector number (0-31 exception, 32-47 IRQ), err_code = the CPU's error code or 0
    uint32_t int_no, err_code;
    // where we were interrupted: the instruction to return to, its code segment and the flags
    uint32_t eip, cs, eflags;                        // pushed by the CPU
};

// a driver's hardware interrupt handler. It runs inside the interrupt with further interrupts
// disabled (interrupt gates clear IF), so it should be short, must not sleep or wait for another
// IRQ (sleep_ms() would fall back to busy waiting), and does NOT send the end of interrupt itself:
// isr_dispatch() does that after the handler returns
typedef void (*irq_handler_t)(struct regs* r);

// true once idt_init() has finished, the boot log shows it as "[ OK ]" or "[FAIL]"
extern bool interrupts_ready;

// load our own flat GDT and reload every segment register to use it
void gdt_init(void);
// fill and load the IDT and remap the PIC, every IRQ starts masked and interrupts stay disabled
void idt_init(void);

// allow IRQ line irq (0-15) to reach the CPU, call after installing its handler
void pic_unmask(uint8_t irq);
// stop IRQ line irq (0-15) reaching the CPU
void pic_mask(uint8_t irq);

// register handler as the C function for IRQ line irq (0-15), replacing any old one, irq >= 16 is ignored
void irq_install_handler(uint8_t irq, irq_handler_t handler);
// remove the handler for IRQ line irq (0-15), the line stays unmasked unless pic_mask() is called too
void irq_uninstall_handler(uint8_t irq);

#endif /* INTERRUPTS_H */

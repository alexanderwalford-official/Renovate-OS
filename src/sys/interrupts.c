/*
    interrupts.c
    GDT, IDT, PIC and the interrupt entry stubs, see interrupts.h.

    This file is where AerOS takes control of the CPU's interrupt machinery. In order:

      1. gdt_init()   builds a 3 entry GDT (null, kernel code, kernel data) using the flat 4GB
                      model and reloads every segment register so the CPU actually uses it.
      2. the stubs    one tiny assembly entry point per vector. The CPU does not tell a handler
                      which vector fired, so each stub pushes its own number before jumping to
                      isr_common_stub, which saves every register as a "struct regs" and calls C.
      3. isr_dispatch() decides what to do: CPU exceptions (0-31) go to the red panic screen,
                      hardware IRQs (32-47) go to the driver that registered for them, and then
                      the PIC is told "end of interrupt" so it can send the next one.
      4. idt_init()   points IDT gates 0-47 at the stubs, remaps the PIC and loads the IDT.

    Key ideas a reader needs:
      - interrupt gate: an IDT entry that disables interrupts (clears IF) on entry, so our
        handlers never interrupt each other. "iret" restores the old EFLAGS and so turns them
        back on when the handler returns.
      - error code: some exceptions (8, 10-14, 17, 21, 29, 30) make the CPU push an extra 32 bit
        value describing the fault, the others do not. The stubs push a fake 0 for the others so
        the stack always has the same layout.
      - EOI: the PIC will not deliver another interrupt of the same or lower priority until it
        receives an "end of interrupt" command.
      - spurious IRQ: a phantom IRQ7 / IRQ15 the PIC produces when a request vanishes before the
        CPU accepts it. It must be ignored (and NOT acknowledged like a real one).

    https://wiki.osdev.org/GDT_Tutorial
    https://wiki.osdev.org/Global_Descriptor_Table
    https://wiki.osdev.org/Interrupt_Descriptor_Table
    https://wiki.osdev.org/8259_PIC
    https://wiki.osdev.org/Exceptions
*/
#include "interrupts.h"
#include "power.h"
#include "../drivers/input/GIO.h"
#include "../drivers/serial/serial.h"
#include "../drivers/video/VGA_linear.h"
#include "../lib/primary_definitions.h"

// I/O ports of the two 8259 PICs. The master handles IRQ 0-7, the slave handles IRQ 8-15 and
// is chained ("cascaded") into the master's IRQ2 line. Each chip has a command port and a
// data port (the data port also reads/writes the chip's interrupt mask register, IMR)
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
// OCW2 "non specific end of interrupt": clears the highest priority IRQ the chip has in service
#define PIC_EOI      0x20
// OCW3 command (bit 3 set) with bits 0-1 = 11: the next read of the command port returns the
// in service register (ISR) instead of the request register (that would be 0x0A)
#define PIC_READ_ISR 0x0B

// ICW = initialisation command word, the 4 byte sequence that (re)programs a PIC
// ICW1 bit 4: "this is ICW1, start initialising". Bits 1 and 3 stay 0, meaning two cascaded
// chips (not a single one) and edge triggered IRQs, which is the standard PC wiring
#define ICW1_INIT    0x10
// ICW1 bit 0: an ICW4 will follow
#define ICW1_ICW4    0x01
// ICW4 bit 0: 8086/88 mode (the alternative is the ancient 8080/8085 mode)
#define ICW4_8086    0x01

// vectors 0-31 belong to the CPU exceptions, so the 16 IRQs go straight after them (32-47).
// The BIOS leaves the master PIC at vector 8, which would make the timer look like a double fault
#define IRQ_BASE_VECTOR    0x20 // IRQ0 -> interrupt 32
// 0x8E = 1000 1110 in binary:
//   bit 7      P = 1      the gate is present
//   bits 5-6   DPL = 00   only ring 0 may trigger it with an "int" instruction
//   bit 4      0          always 0 for interrupt / trap gates
//   bits 0-3   0xE        32 bit interrupt gate (clears IF on entry, 0xF would be a trap gate that doesn't)
#define IDT_INTERRUPT_GATE 0x8E // present, ring 0, 32 bit interrupt gate

// one 8 byte GDT descriptor. The base and limit are chopped into pieces because the 386 had to
// stay compatible with the 286's 24 bit descriptors, so the extra bits were squeezed in around
// them. packed makes sure the compiler never adds padding, the CPU reads these exact bytes
typedef struct __attribute__((packed)) {
    uint16_t limit_low;   // segment limit bits 0-15
    uint16_t base_low;    // segment start address bits 0-15
    uint8_t  base_mid;    // base bits 16-23
    uint8_t  access;      // present, privilege ring, code/data type, readable/writable (see gdt_init)
    uint8_t  granularity; // high nibble: flags (4KB granularity, 32 bit), low nibble: limit bits 16-19
    uint8_t  base_high;   // base bits 24-31
} gdt_entry_t;

// one 8 byte IDT gate: which code segment and address to jump to for a vector, and how
typedef struct __attribute__((packed)) {
    uint16_t offset_low;  // handler address bits 0-15
    uint16_t selector;    // code segment selector
    uint8_t  zero;        // always 0
    uint8_t  flags;       // type and attributes
    uint16_t offset_high; // handler address bits 16-31
} idt_entry_t;

// the operand of lgdt / lidt, the CPU copies it so it only has to live until the instruction runs
// (6 bytes: without packed the compiler would put base at offset 4 instead of 2 and the CPU would
// load a garbage address)
typedef struct __attribute__((packed)) {
    uint16_t limit; // size of the table in bytes minus 1 (the offset of its last valid byte)
    uint32_t base;  // address of the first entry
} table_ptr_t;

// null, kernel code and kernel data. A TSS and ring 3 (user) segments will be needed here one
// day for user mode programs
static gdt_entry_t gdt[3];
// one gate for every possible vector (256 * 8 bytes = 2KB). Being static it starts zeroed in .bss,
// and a zeroed gate is simply "not present"
static idt_entry_t idt[256];
// the C handler for each IRQ line, 0 (NULL) means nobody has claimed it
static irq_handler_t irq_handlers[16];

// set at the end of idt_init(), see interrupts.h
bool interrupts_ready = false;

// fill in GDT entry index, scattering base and limit into the bit fields the CPU expects.
// limit is a 20 bit value (only bits 0-19 are used), only the high nibble of granularity is used
// (the low nibble is replaced by limit bits 16-19)
static void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity) {
    gdt[index].base_low = (uint16_t)(base & 0xFFFF);
    gdt[index].base_mid = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].base_high = (uint8_t)((base >> 24) & 0xFF);
    gdt[index].limit_low = (uint16_t)(limit & 0xFFFF);
    // the low nibble holds limit bits 16-19, the high nibble the flags
    gdt[index].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (granularity & 0xF0));
    gdt[index].access = access;
}

// flat memory model: one code and one data segment covering all 4GB
//
// the access bytes:
//   0x9A = 1001 1010: present, ring 0, code/data descriptor, executable, non conforming, readable
//   0x92 = 1001 0010: present, ring 0, code/data descriptor, data, grows up, writable
// the granularity byte 0xCF:
//   high nibble 0xC = 1100: limit counted in 4KB pages (G) and 32 bit segment (D/B)
//   low nibble  0xF       : limit bits 16-19, so the limit is 0xFFFFF pages * 4KB = 4GB
// the "accessed" bit (bit 0 of access) starts at 0, the CPU sets it itself
void gdt_init(void) {
    gdt_set_entry(0, 0, 0, 0, 0);                // null descriptor
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // kernel code (0x08)
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // kernel data (0x10)

    table_ptr_t gdt_ptr = { sizeof(gdt) - 1, (uint32_t)(uintptr_t)gdt };
    // lgdt only tells the CPU where the table is. Every segment register keeps a hidden cached
    // copy of its old descriptor until it is loaded again, so we have to reload them all:
    //   - CS cannot be written with mov, the only way is a far jump (ljmp selector, offset).
    //     "$1f" means "the label 1 forwards", i.e. the very next line, so we jump to where we
    //     already are, just with CS = 0x08
    //   - then 0x10 goes into every data segment register, including SS. That is safe in the
    //     middle of a function because the old and new stack segments are both flat, so the
    //     stack does not move
    // eax is listed as clobbered because we use ax, "memory" stops the compiler moving memory
    // accesses across the switch
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

// move IRQ 0-7 to interrupts 0x20-0x27 and IRQ 8-15 to 0x28-0x2F, all IRQs start masked
//
// after ICW1 arrives on the command port, each PIC expects exactly three more bytes on its data
// port, in this order:
//   ICW2  the vector offset: the chip adds its IRQ number (0-7) to this, so it must be a multiple of 8
//   ICW3  how the chips are chained: the master gets a bit mask of the lines that have a slave on
//         them, the slave gets the number of the master line it is plugged into
//   ICW4  extra mode bits, we only need 8086 mode
// the two chips are interleaved, ICW by ICW, which is the order the osdev wiki uses.
// io_wait() between writes gives old (real) PICs time to process each byte
static void pic_remap(void) {
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC1_DATA, IRQ_BASE_VECTOR); io_wait();     // master PIC vector offset
    outb(PIC2_DATA, IRQ_BASE_VECTOR + 8); io_wait(); // slave PIC vector offset
    // ICW3: 4 = binary 0000 0100, a bit mask (bit 2), whereas the slave wants a plain number (2)
    outb(PIC1_DATA, 4); io_wait();                   // slave PIC is on IRQ2
    outb(PIC2_DATA, 2); io_wait();                   // slave PIC cascade identity
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    // the mask registers (OCW1): a 1 bit blocks that line. Drivers unmask their own line with
    // pic_unmask() once their handler is installed, so nothing fires at a missing handler.
    // IRQ2 has to stay open, every slave IRQ (8-15) reaches the CPU through it
    outb(PIC1_DATA, 0xFB); // mask everything except IRQ2 (the cascade to the slave)
    outb(PIC2_DATA, 0xFF);
}

// allow an IRQ line to fire
//
// IRQ 0-7 live in the master's mask register and 8-15 in the slave's, irq & 7 is the bit position
// inside that chip. This is a read-modify-write of the mask, so it should not race with an
// interrupt handler that also changes masks (none do, it is only called during start up)
void pic_unmask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(1 << (irq & 7));
    // clear the bit: 0 = allowed
    outb(port, inb(port) & (uint8_t)~bit);
}

// block an IRQ line
//
// the mirror image of pic_unmask(): set the line's bit in the right chip's mask register
void pic_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(1 << (irq & 7));
    // set the bit: 1 = blocked
    outb(port, inb(port) | bit);
}

// read the "in service" register of both PICs (used to detect spurious IRQs)
//
// the ISR has a bit set for every IRQ the chip has delivered to the CPU but not yet had an EOI
// for. Returns slave << 8 | master, so bit n of the result is IRQ n
static uint16_t pic_read_isr(void) {
    // ask both chips to show their ISR on the next command port read, then read them back
    outb(PIC1_COMMAND, PIC_READ_ISR);
    outb(PIC2_COMMAND, PIC_READ_ISR);
    return (uint16_t)((inb(PIC2_COMMAND) << 8) | inb(PIC1_COMMAND));
}

// let a driver claim an IRQ line, e.g. the timer uses 0 and the keyboard uses 1. There is one
// handler per line (no sharing), a second call replaces the first. Out of range lines are ignored.
// Storing a pointer is a single aligned 32 bit write, so even an IRQ arriving mid call sees
// either the old handler or the new one, never half of each
void irq_install_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

// forget the handler for an IRQ line. If the line is still unmasked its interrupts keep arriving,
// isr_dispatch() then just sends the EOI and carries on
void irq_uninstall_handler(uint8_t irq) {
    if (irq < 16) {
        irq_handlers[irq] = 0;
    }
}

/*
    Interrupt entry stubs (inline assembly).
    Every stub pushes a dummy error code (if the CPU did not push one) and the interrupt number,
    then jumps to a common stub that saves all registers and calls isr_dispatch() in C.

    INT_STUB(name, vector, push_error_code) expands to a top level __asm__ block that:
      - switches to the .text section for a moment (.pushsection / .popsection put back whatever
        section GCC was in, so the compiler's own output is not disturbed)
      - defines a global label called name (#name turns the macro argument into a string)
      - pastes in push_error_code, which is either "pushl $0" or nothing at all
      - pushes the vector number ("pushl $" #vector becomes e.g. "pushl $14")
      - jumps to isr_common_stub
    and finally declares "extern void name(void)" so the C code can take the label's address
    for the IDT. The extern also swallows the ";" written after each use of the macro.

    ISR_NOERR(n) for exceptions where the CPU pushes nothing, so we push a fake error code of 0
    ISR_ERR(n)   for exceptions where the CPU already pushed a real error code
    IRQ_STUB(n, vector) for hardware IRQ n, which arrives as interrupt vector (32 + n). The PIC
                 never pushes an error code, so these always push a fake 0
    Getting ISR_ERR / ISR_NOERR wrong for a vector would shift the whole stack frame by 4 bytes
    and "iret" would return to a nonsense address, so the table below has to match the CPU exactly.
*/
#define INT_STUB(name, vector, push_error_code) \
    __asm__(".pushsection .text\n" \
            ".global " #name "\n" \
            #name ":\n\t" \
            push_error_code \
            "pushl $" #vector "\n\t" \
            "jmp isr_common_stub\n" \
            ".popsection\n"); \
    extern void name(void)

#define ISR_NOERR(n)        INT_STUB(isr##n, n, "pushl $0\n\t")
#define ISR_ERR(n)          INT_STUB(isr##n, n, "")
#define IRQ_STUB(n, vector) INT_STUB(irq##n, vector, "pushl $0\n\t")

// the code every stub jumps to. On arrival the stack holds (top first): int_no, err_code, eip,
// cs, eflags. Step by step:
//   - pusha + 4 segment pushes save the interrupted code's registers, completing the struct regs
//   - the data segment registers are loaded with the kernel data selector 0x10. Today everything
//     already runs with 0x10, but once user mode exists an interrupt may arrive while ring 3
//     segments are loaded
//   - cld because the C calling convention promises the direction flag is clear on function
//     entry, and we may have interrupted code in the middle of a backwards "std; rep movs"
//   - esp now points at the gs field, i.e. at the start of struct regs, so pushing esp passes
//     that pointer as isr_dispatch()'s one argument (cdecl: arguments go on the stack)
//   - after the call: drop the argument, restore everything in reverse order, drop int_no and
//     err_code (iret expects esp to point at eip and would never remove the error code itself)
//   - iret pops eip, cs and eflags, which also restores the interrupt flag
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
// the ones with an error code are: 8 double fault (always 0), 10 invalid TSS, 11 segment not
// present, 12 stack segment fault, 13 general protection fault (the selector involved, often 0),
// 14 page fault (what kind of access failed), 17 alignment check (always 0), 21 control
// protection, 29 VMM communication and 30 security exception
ISR_NOERR(0);  ISR_NOERR(1);  ISR_NOERR(2);  ISR_NOERR(3);
ISR_NOERR(4);  ISR_NOERR(5);  ISR_NOERR(6);  ISR_NOERR(7);
ISR_ERR(8);    ISR_NOERR(9);  ISR_ERR(10);   ISR_ERR(11);
ISR_ERR(12);   ISR_ERR(13);   ISR_ERR(14);   ISR_NOERR(15);
ISR_NOERR(16); ISR_ERR(17);   ISR_NOERR(18); ISR_NOERR(19);
ISR_NOERR(20); ISR_ERR(21);   ISR_NOERR(22); ISR_NOERR(23);
ISR_NOERR(24); ISR_NOERR(25); ISR_NOERR(26); ISR_NOERR(27);
ISR_NOERR(28); ISR_ERR(29);   ISR_ERR(30);   ISR_NOERR(31);

// hardware IRQs (after remapping)
// the usual PC wiring: 0 PIT timer, 1 PS/2 keyboard, 2 cascade from the slave PIC (never fires
// itself), 3 COM2, 4 COM1, 5 LPT2 / sound card, 6 floppy, 7 LPT1 (and spurious), 8 CMOS real
// time clock, 9-11 free / ACPI / PCI, 12 PS/2 mouse, 13 FPU, 14 primary ATA, 15 secondary ATA
IRQ_STUB(0, 32);  IRQ_STUB(1, 33);  IRQ_STUB(2, 34);  IRQ_STUB(3, 35);
IRQ_STUB(4, 36);  IRQ_STUB(5, 37);  IRQ_STUB(6, 38);  IRQ_STUB(7, 39);
IRQ_STUB(8, 40);  IRQ_STUB(9, 41);  IRQ_STUB(10, 42); IRQ_STUB(11, 43);
IRQ_STUB(12, 44); IRQ_STUB(13, 45); IRQ_STUB(14, 46); IRQ_STUB(15, 47);

// the name of each CPU exception, indexed by vector number, shown on the panic screen
static const char* const exception_names[32] = {
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
//
// r is the saved CPU state, r->int_no is always 0-31 here. The screen shows:
//   EIP     the instruction that faulted (for "trap" exceptions such as breakpoint and
//           overflow it is the instruction after it)
//   CS / EFLAGS / ERR   the code segment, flags and the exception's error code (0 if it has none)
//   EAX-EDX the general registers at the moment of the crash, often a clue to a bad pointer
// the message is also sent to COM1 so it shows up in the QEMU serial log. Interrupts are already
// off (we came in through an interrupt gate) and sys_halt() never returns
static __attribute__((noreturn)) void exception_panic(struct regs* r) {
    const char* name = exception_names[r->int_no];
    // one text mode row is 80 characters wide
    char line[80];

    clear(RED);
    puts(5, 7, BLACK, RED, "==================================");
    puts(5, 9, BLACK, RED, ">> SYSTEM EXCEPTION <<");
    puts(5, 10, BLACK, RED, "The system encountered an error and was forced to stop.");
    puts(5, 11, BLACK, RED, "Message is as followed:");
    sfmt(line, sizeof(line), "[ ! ] CPU EXCEPTION %u >> %s", r->int_no, name);
    puts(5, 12, BLACK, RED, line);
    sfmt(line, sizeof(line), "EIP=%08x  CS=%04x  EFLAGS=%08x  ERR=%08x", r->eip, r->cs, r->eflags, r->err_code);
    puts(5, 14, BLACK, RED, line);
    sfmt(line, sizeof(line), "EAX=%08x  EBX=%08x  ECX=%08x  EDX=%08x", r->eax, r->ebx, r->ecx, r->edx);
    puts(5, 15, BLACK, RED, line);
    // for a page fault the CPU puts the address it was trying to access in control register CR2.
    // Paging is not switched on yet so this cannot happen today, but it is ready for when it is
    if (r->int_no == 14) {
        uint32_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        sfmt(line, sizeof(line), "FAULTING ADDRESS (CR2)=%08x", cr2);
        puts(5, 16, BLACK, RED, line);
    }
    puts(5, 18, BLACK, RED, "The system has been halted, please restart your computer.");

    serial_write("\n*** CPU EXCEPTION: ");
    serial_write(name);
    serial_write(" ***\n");

    sys_halt();
}

// called by isr_common_stub for every interrupt (not static, it is only referenced from the assembly)
//
// r points at the registers saved on the stack (see struct regs). Exceptions never come back from
// here. For IRQs the order matters: filter out spurious IRQs, run the driver's handler, then
// send the EOI. The handler runs before the EOI, but that is fine because interrupts are off
// for the whole time anyway. A handler that never returned would also never send the EOI, and
// that IRQ line (plus every lower priority one) would go quiet
void isr_dispatch(struct regs* r) {
    if (r->int_no < 32) {
        exception_panic(r);
    }

    // everything else is a hardware IRQ, idt_init() installs no other gates
    // (so int_no is 32-47 and irq is 0-15, a safe index into irq_handlers)
    uint8_t irq = (uint8_t)(r->int_no - IRQ_BASE_VECTOR);

    // spurious IRQs (electrical noise) must not be acknowledged
    // if a request disappears before the CPU accepts it, the PIC still has to hand over a vector
    // and uses its lowest priority line: IRQ7 on the master, IRQ15 on the slave. A real one
    // has its bit set in the in service register, a spurious one does not. An EOI for a spurious
    // IRQ would wrongly end some other IRQ that really is in service
    if (irq == 7 && !(pic_read_isr() & (1 << 7))) {
        return;
    }
    if (irq == 15 && !(pic_read_isr() & (1 << 15))) {
        outb(PIC1_COMMAND, PIC_EOI); // the master still saw the cascade IRQ
        return;
    }

    // hand the interrupt to whichever driver claimed this line (if any)
    if (irq_handlers[irq]) {
        irq_handlers[irq](r);
    }

    // end of interrupt, IRQ 8-15 came through the slave so both PICs need it
    // (the slave for the device itself, the master for its IRQ2 cascade line)
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

// point IDT gate vector at an entry stub. The CPU loads CS with KERNEL_CODE_SELECTOR and jumps to
// the stub's address, using an interrupt gate so interrupts are off while the handler runs
static void idt_set_gate(uint8_t vector, void (*stub)(void)) {
    uint32_t handler = (uint32_t)(uintptr_t)stub;
    // the 32 bit address is split in two halves around the selector and flags
    idt[vector].offset_low = (uint16_t)(handler & 0xFFFF);
    idt[vector].offset_high = (uint16_t)(handler >> 16);
    idt[vector].selector = KERNEL_CODE_SELECTOR;
    idt[vector].zero = 0;
    idt[vector].flags = IDT_INTERRUPT_GATE;
}

// fill the IDT, remap the PIC and load the IDT (interrupts stay disabled until interrupts_enable())
//
// must run after gdt_init(), because every gate refers to the kernel code selector 0x08
void idt_init(void) {
    // tables of the stub addresses, so the gates can be filled in with a loop
    // (static const so they live in read only data instead of being rebuilt on the stack)
    static void (*const exception_stubs[32])(void) = {
        isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
        isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    };
    static void (*const irq_stubs[16])(void) = {
        irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7,
        irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
    };

    // every other vector stays zeroed (not present), so using one raises a general protection fault
    // (which then lands on the panic screen through gate 13)
    for (int i = 0; i < 32; i++) {
        idt_set_gate((uint8_t)i, exception_stubs[i]);
    }
    for (int i = 0; i < 16; i++) {
        idt_set_gate((uint8_t)(IRQ_BASE_VECTOR + i), irq_stubs[i]);
    }

    pic_remap();

    // limit = 256 * 8 - 1 = 2047, the whole table
    table_ptr_t idt_ptr = { sizeof(idt) - 1, (uint32_t)(uintptr_t)idt };
    // unlike lgdt there is nothing to reload afterwards, the CPU reads the IDT fresh on every interrupt
    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));

    // reported by the boot log and SYSINFO
    interrupts_ready = true;
}

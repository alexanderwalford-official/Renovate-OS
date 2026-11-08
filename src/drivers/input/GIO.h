/*
    Alexander Walford 2023
    General purpose I/O driver for interacting with devices.

    Devices on x86 are reached in two different ways:
    - I/O ports (inb / outb ...): a separate 64KB address space used by the PIC, PIT,
      keyboard controller, PC speaker, CMOS clock, ATA controller and so on.
    - Memory mapped I/O (get_byte / set_byte ...): normal memory addresses that are
      wired to a device, such as the VGA text buffer at 0xB8000.

    A bit more detail, because nearly every driver in Renovate OS is built on this file:

    I/O ports
      The CPU has a second, completely separate address space just for talking to hardware.
      It is 16 bits wide (ports 0x0000 - 0xFFFF) and can only be reached with the special
      IN and OUT instructions; an ordinary pointer can never touch it. Each port is wired to
      a register inside some chip: write a byte to port 0x43 and it lands in the timer chip's
      command register, read port 0x60 and you get the last scancode from the keyboard.
      Port I/O is slow (hundreds of CPU cycles on real hardware, and in a virtual machine every
      single IN / OUT traps into the emulator), so drivers try not to do more of it than needed.

    Memory mapped I/O (MMIO)
      Some devices instead answer at normal physical memory addresses. Renovate OS runs without
      paging, so a physical address can be used directly as a pointer. The pointer has to be
      volatile: otherwise the compiler is allowed to assume memory only changes when our code
      changes it, and may merge, reorder or delete reads and writes that the device depends on.

    Inline assembly cheat sheet (GCC syntax, AT&T operand order = source first, destination last)
      "a"   the value lives in AL / AX / EAX (IN and OUT can only move data through the A register)
      "d"   the value lives in DX
      "N"   an unsigned 8 bit constant, so "Nd" lets the compiler use "outb %al, $0x80" when the port
            is a known number 0-255, and fall back to putting the port in DX otherwise
      "=a"  an output that comes back in the A register
      "+D"  EDI is read AND written by the instruction ("+" = in/out), same for "+S" (ESI), "+c" (ECX)
      "memory" clobber: tells the compiler that memory changed (or must be up to date) behind its
            back, which also stops it moving loads / stores across the instruction

    Every function here is "static inline" in a header so each driver gets its own copy and the
    compiler can boil a call down to the single instruction it wraps.

    https://wiki.osdev.org/Port_IO
    https://wiki.osdev.org/Inline_Assembly/Examples
*/
#ifndef GIO_H
#define GIO_H

#include <stdint.h>

// write one byte to an I/O port ("outb %al, %dx")
// port: the port number (0x0000 - 0xFFFF), value: the byte to send
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

// read one byte from an I/O port ("inb %dx, %al") and return it
// gotcha: reading some ports has side effects (e.g. reading 0x60 takes the scancode out of the
// keyboard controller), which is why this is volatile and never optimised away
static inline uint8_t inb(uint16_t port) {
    uint8_t result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

// write a 16 bit word to an I/O port (used for 16 bit wide registers such as the ATA data port)
static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

// read a 16 bit word from an I/O port
static inline uint16_t inw(uint16_t port) {
    uint16_t result;
    __asm__ volatile ("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

// write a 32 bit double word to an I/O port (e.g. PCI configuration space at 0xCF8 / 0xCFC)
static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

// read a 32 bit double word from an I/O port
static inline uint32_t inl(uint16_t port) {
    uint32_t result;
    __asm__ volatile ("inl %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

// read count 16 bit words from a port into buffer in one go (e.g. a disk sector)
// "rep insw" is a string instruction: it reads a word from port DX, stores it at [EDI], adds 2 to
// EDI and subtracts 1 from ECX, repeating until ECX is 0. That is why buffer / count are "+D" / "+c"
// (the CPU changes those registers) and why "memory" is clobbered (the buffer is written behind
// the compiler's back). EDI moves forwards because the C calling convention guarantees the
// direction flag is clear.
static inline void insw(uint16_t port, void* buffer, uint32_t count) {
    __asm__ volatile ("rep insw" : "+D"(buffer), "+c"(count) : "d"(port) : "memory");
}

// write count 16 bit words from buffer to a port in one go
// the mirror image of insw: "rep outsw" reads each word from [ESI] and sends it to port DX
static inline void outsw(uint16_t port, const void* buffer, uint32_t count) {
    __asm__ volatile ("rep outsw" : "+S"(buffer), "+c"(count) : "d"(port) : "memory");
}

// wait roughly 1-4 microseconds by writing to an unused port (gives slow devices time to respond)
// port 0x80 is the BIOS POST code port: nothing important listens to it after boot, and an OUT to
// it takes about as long as a trip over the old ISA bus, which makes it a handy tiny delay
static inline void io_wait(void) {
    outb(0x80, 0);
}

// read a byte from a memory mapped address (volatile, so the read really happens every time)
static inline uint8_t get_byte(uintptr_t addr) {
    return *(volatile uint8_t*)addr;
}

// write a byte to a memory mapped address
static inline void set_byte(uintptr_t addr, uint8_t value) {
    *(volatile uint8_t*)addr = value;
}

// read a 16 bit word from a memory mapped address (e.g. one VGA text cell)
static inline uint16_t get_word(uintptr_t addr) {
    return *(volatile uint16_t*)addr;
}

// write a 16 bit word to a memory mapped address
static inline void set_word(uintptr_t addr, uint16_t value) {
    *(volatile uint16_t*)addr = value;
}

// "sti" = set the interrupt flag: from now on the CPU accepts hardware interrupts (IRQs)
// only call this once the IDT and PIC are set up, or the first IRQ will crash the machine
// the "memory" clobber stops the compiler moving memory accesses to the other side of the sti
static inline void interrupts_enable(void) {
    __asm__ volatile ("sti" ::: "memory");
}

// "cli" = clear the interrupt flag: hardware interrupts are held back until the next sti
// (CPU exceptions and NMIs still happen), used to protect code that must not be interrupted
static inline void interrupts_disable(void) {
    __asm__ volatile ("cli" ::: "memory");
}

// returns 1 if hardware interrupts are currently enabled (IF flag set)
// there is no instruction that moves EFLAGS straight into a register, so we push it onto the
// stack ("pushfl") and pop it back off into a register ("popl"). Bit 9 (0x200) is IF.
static inline int interrupts_enabled(void) {
    uint32_t flags;
    __asm__ volatile ("pushfl\n\tpopl %0" : "=r"(flags));
    return (flags & 0x200) != 0;
}

// sleep the CPU until the next interrupt arrives
// "hlt" stops the CPU (saving power and, in a VM, host CPU time) until an interrupt wakes it.
// gotcha: with interrupts disabled only an NMI can wake it, so hlt with IF clear hangs forever
static inline void cpu_idle(void) {
    __asm__ volatile ("hlt");
}

#endif /* GIO_H */

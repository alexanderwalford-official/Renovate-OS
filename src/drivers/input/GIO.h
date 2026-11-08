/*
    Alexander Walford 2023
    General purpose I/O driver for interacting with devices.

    Devices on x86 are reached in two different ways:
    - I/O ports (inb / outb ...): a separate 64KB address space used by the PIC, PIT,
      keyboard controller, PC speaker, CMOS clock, ATA controller and so on.
    - Memory mapped I/O (get_byte / set_byte ...): normal memory addresses that are
      wired to a device, such as the VGA text buffer at 0xB8000.
*/
#ifndef GIO_H
#define GIO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t result;
    __asm__ volatile ("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t result;
    __asm__ volatile ("inl %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

// read count 16 bit words from a port into buffer in one go (e.g. a disk sector)
static inline void insw(uint16_t port, void* buffer, uint32_t count) {
    __asm__ volatile ("rep insw" : "+D"(buffer), "+c"(count) : "d"(port) : "memory");
}

// write count 16 bit words from buffer to a port in one go
static inline void outsw(uint16_t port, const void* buffer, uint32_t count) {
    __asm__ volatile ("rep outsw" : "+S"(buffer), "+c"(count) : "d"(port) : "memory");
}

// wait roughly 1-4 microseconds by writing to an unused port (gives slow devices time to respond)
static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline uint8_t get_byte(uintptr_t addr) {
    return *(volatile uint8_t*)addr;
}

static inline void set_byte(uintptr_t addr, uint8_t value) {
    *(volatile uint8_t*)addr = value;
}

static inline uint16_t get_word(uintptr_t addr) {
    return *(volatile uint16_t*)addr;
}

static inline void set_word(uintptr_t addr, uint16_t value) {
    *(volatile uint16_t*)addr = value;
}

static inline void interrupts_enable(void) {
    __asm__ volatile ("sti" ::: "memory");
}

static inline void interrupts_disable(void) {
    __asm__ volatile ("cli" ::: "memory");
}

// returns 1 if hardware interrupts are currently enabled (IF flag set)
static inline int interrupts_enabled(void) {
    uint32_t flags;
    __asm__ volatile ("pushfl\n\tpopl %0" : "=r"(flags));
    return (flags & 0x200) != 0;
}

// sleep the CPU until the next interrupt arrives
static inline void cpu_idle(void) {
    __asm__ volatile ("hlt");
}

#endif /* GIO_H */

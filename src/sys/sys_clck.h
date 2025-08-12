/*
    SYS_CLCK_H
    This class file is responsible for handling all system sensetive operations.

    TO DO:
    Replace with time.h and clock_t method - this will enable realtime processes not per-processor dependant timings.
*/

#include <stdint.h>
#include <stddef.h>
#include "../lib/primary_definitions.h"
#include "../drivers/video/VGA_linear.h"

#ifndef SYS_CLOCK_H
#define SYS_CLOCK_H

bool sys_threads_status[4] = {true, false, false, false}; // eventual support for quad threads, only state 0 is used currently

#define PIT_FREQUENCY 1193182   // PIT input clock frequency
#define PIT_COMMAND   0x43      // Command register port
#define PIT_CHANNEL0  0x40      // Channel 0 data port
//#define TIMER_VECTOR 0x08 // real mode
#define TIMER_VECTOR 0x20 // protected mode
#define PIC_CMD 0x20

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01
#define ICW4_8086    0x01
#define PIC1_DATA 0x21

typedef void (*irq_handler_t)(void);
irq_handler_t old_timer_handler = 0;
volatile unsigned long ticks = 0;

bool UseOld = true;


#define FP_OFF(fp) ((uint16_t)((uint32_t)(void *)(fp) & 0xFFFFu))
#define FP_SEG(fp) ((uint16_t)(((uint32_t)(void *)(fp) >> 16) & 0xFFFFu))


void unmask_irq0(void) {
    uint8_t mask = inb(PIC1_DATA);  // read current mask
    mask &= ~(1 << 0);              // clear bit 0 to unmask IRQ0
    outb(PIC1_DATA, mask);          // write mask back
}

irq_handler_t get_vector(uint8_t vector) {
    uint16_t *ivt_entry = (uint16_t *)(uintptr_t)(vector * 4);

    uint16_t off = ivt_entry[0];
    uint16_t seg = ivt_entry[1];
    irq_handler_t h = (irq_handler_t) ((uint32_t)seg << 16 | off);
    return h;
}

void set_vector(uint8_t vector, irq_handler_t handler) {
    uint16_t *ivt_entry = (uint16_t *)(uintptr_t)(vector * 4);

    ivt_entry[0] = FP_OFF(handler);
    ivt_entry[1] = FP_SEG(handler);
}

void pit_set_frequency(uint16_t hz) {
    if (hz == 0) return;
    uint16_t divisor = (uint16_t)(PIT_FREQUENCY / hz);
    outb(PIT_COMMAND, 0x36);  // set PIT mode: channel 0, lobyte/hibyte, mode 3 (square wave)
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}

void timer_handler(void) {
    ticks++;  // increment global tick count
    outb(PIC_CMD, 0x20);  // send End Of Interrupt (EOI) to PIC if no chaining
}

void hook_timer(uint16_t hz) {
    old_timer_handler = get_vector(TIMER_VECTOR); // save current handler
    set_vector(TIMER_VECTOR, timer_handler); // install our handler
    pit_set_frequency(hz); // set PIT frequency
}

void unhook_timer(void) {
    if (old_timer_handler) {
        set_vector(TIMER_VECTOR, old_timer_handler); // restore old handler
        old_timer_handler = 0;
    }
}


void sleep_ms(unsigned long ms) {
    unsigned long target = ticks + ms;
    while (ticks < target) {
        __asm__ ("nop"); 
    }
}

void pic_remap(void) {
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);

    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);

    outb(PIC1_DATA, 0x20); // master PIC vector offset (IRQ0 mapped to 0x20)
    outb(PIC2_DATA, 0x28); // slave PIC vector offset

    outb(PIC1_DATA, 4);
    outb(PIC2_DATA, 2);

    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    outb(PIC1_DATA, a1 & ~0x01); // unmask IRQ0 (timer)
    outb(PIC2_DATA, a2);
}


// haults a thread for the desired time, we will later add a argument for thread ID and then will check if it is in use
void clck_hang_old (float t, float lt) {
    sys_threads_status[0] = false;
    lt = lt + 0.00013; 
    if (lt < t) { // there is a reason why this is not a branchless method! It's slower..
        clck_hang_old(t, lt);
    }
    else {
        sys_threads_status[0] = true; 
    }
}

void clck_hang(long time) {
    if (UseOld == true) {
        clck_hang_old(time, 0);
    }
    else {
        pic_remap();
        unmask_irq0();
        hook_timer(1000);  // sets vector and PIT frequency
        __asm__ ("sti");
        sleep_ms(time);
        unhook_timer();
    }
    
}


#endif
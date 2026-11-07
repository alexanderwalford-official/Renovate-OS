/*
    SYS_CLCK_H
    This class file is responsible for handling all system time sensitive operations.

    - The PIT (programmable interval timer) fires IRQ0 1000 times a second, so "ticks" counts
      milliseconds since the timer started. Delays no longer depend on the speed of the processor.
    - The CMOS real time clock (RTC) gives the current date and time.

    https://wiki.osdev.org/Programmable_Interval_Timer
    https://wiki.osdev.org/CMOS
*/
#ifndef SYS_CLOCK_H
#define SYS_CLOCK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../lib/primary_definitions.h"
#include "../drivers/input/GIO.h"
#include "interrupts.h"

bool sys_threads_status[4] = {true, false, false, false}; // eventual support for quad threads, only state 0 is used currently

#define PIT_FREQUENCY 1193182   // PIT input clock frequency
#define PIT_COMMAND   0x43      // Command register port
#define PIT_CHANNEL0  0x40      // Channel 0 data port
#define PIT_HZ        1000      // timer interrupts per second (1 tick = 1 millisecond)

#define CMOS_ADDRESS  0x70
#define CMOS_DATA     0x71

volatile uint32_t ticks = 0; // milliseconds since clck_init()
bool clck_ready = false;

// set how often the PIT fires IRQ0
void pit_set_frequency(uint32_t hz) {
    if (hz == 0) return;
    uint32_t divisor = PIT_FREQUENCY / hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;
    outb(PIT_COMMAND, 0x36);  // set PIT mode: channel 0, lobyte/hibyte, mode 3 (square wave)
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}

// IRQ0 handler, the end of interrupt is sent by isr_dispatch
void timer_handler(struct regs* r) {
    (void)r;
    ticks++;  // increment global tick count
}

// start the system timer (requires idt_init() first)
void clck_init(void) {
    pit_set_frequency(PIT_HZ);
    irq_install_handler(0, timer_handler);
    pic_unmask(0);
    clck_ready = true;
}

// milliseconds since the timer started
uint32_t clck_uptime_ms(void) {
    return ticks;
}

// wait for the given number of milliseconds
void sleep_ms(uint32_t ms) {
    if (clck_ready && interrupts_enabled()) {
        uint32_t start = ticks;
        while ((uint32_t)(ticks - start) < ms) {
            cpu_idle(); // sleep until the next interrupt instead of burning the CPU
        }
    } else {
        // fallback before the timer is running: each port write takes roughly 1 microsecond
        for (uint32_t i = 0; i < ms; i++) {
            for (int j = 0; j < 1000; j++) {
                io_wait();
            }
        }
    }
}

// halts a thread for the desired time (milliseconds), we will later add a argument for thread ID and then will check if it is in use
void clck_hang(long time) {
    if (time <= 0) {
        return;
    }
    sys_threads_status[0] = false;
    sleep_ms((uint32_t)time);
    sys_threads_status[0] = true;
}

/* ---------------- real time clock ---------------- */

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} rtc_time_t;

uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDRESS, reg);
    io_wait();
    return inb(CMOS_DATA);
}

bool rtc_update_in_progress(void) {
    return (cmos_read(0x0A) & 0x80) != 0;
}

void rtc_read_raw(rtc_time_t* t, uint8_t* century) {
    // wait (with a limit) for the RTC to finish any update so we do not read half old, half new values
    for (int i = 0; i < 100000 && rtc_update_in_progress(); i++) {
    }
    t->second = cmos_read(0x00);
    t->minute = cmos_read(0x02);
    t->hour = cmos_read(0x04);
    t->day = cmos_read(0x07);
    t->month = cmos_read(0x08);
    t->year = cmos_read(0x09);
    *century = cmos_read(0x32);
}

uint8_t bcd_to_binary(uint8_t value) {
    return (uint8_t)((value & 0x0F) + ((value >> 4) * 10));
}

// read the current date and time from the CMOS clock
void rtc_read(rtc_time_t* t) {
    rtc_time_t last;
    uint8_t century, last_century;

    // read until two reads in a row match, so the value cannot change half way through
    rtc_read_raw(t, &century);
    for (int tries = 0; tries < 10; tries++) {
        last = *t;
        last_century = century;
        rtc_read_raw(t, &century);
        if (memcmp(&last, t, sizeof(rtc_time_t)) == 0 && last_century == century) {
            break;
        }
    }

    uint8_t status_b = cmos_read(0x0B);
    bool pm = (t->hour & 0x80) != 0;
    t->hour &= 0x7F;

    // convert from BCD if needed
    if (!(status_b & 0x04)) {
        t->second = bcd_to_binary(t->second);
        t->minute = bcd_to_binary(t->minute);
        t->hour = bcd_to_binary(t->hour);
        t->day = bcd_to_binary(t->day);
        t->month = bcd_to_binary(t->month);
        t->year = bcd_to_binary((uint8_t)t->year);
        century = bcd_to_binary(century);
    }

    // convert 12 hour clock to 24 hour clock if needed
    if (!(status_b & 0x02)) {
        if (pm && t->hour < 12) t->hour = (uint8_t)(t->hour + 12);
        if (!pm && t->hour == 12) t->hour = 0;
    }

    // add the century (the century register is not on every machine, assume 20xx without it)
    if (century >= 19 && century <= 21) {
        t->year = (uint16_t)(century * 100 + t->year);
    } else {
        t->year = (uint16_t)(2000 + t->year);
    }
}

// format the current date & time as "DD-MM-YY--HH:MM:SS"
char* rtc_datetime(char* out, size_t size) {
    rtc_time_t t;
    rtc_read(&t);
    snprintf(out, size, "%02u-%02u-%02u--%02u:%02u:%02u",
             t.day, t.month, t.year % 100, t.hour, t.minute, t.second);
    return out;
}

#endif

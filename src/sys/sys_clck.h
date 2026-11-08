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

#define PIT_FREQUENCY 1193182   // PIT input clock frequency
#define PIT_COMMAND   0x43      // Command register port
#define PIT_CHANNEL0  0x40      // Channel 0 data port
#define PIT_HZ        1000      // timer interrupts per second (1 tick = 1 millisecond)

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} rtc_time_t;

extern bool sys_threads_status[4]; // eventual support for quad threads, only state 0 is used currently
extern bool clck_ready;

// start the system timer (requires idt_init() first)
void clck_init(void);

// milliseconds since the timer started
uint32_t clck_uptime_ms(void);

// wait for the given number of milliseconds
void sleep_ms(uint32_t ms);

// halts a thread for the desired time (milliseconds), we will later add a argument for thread ID and then will check if it is in use
void clck_hang(long time);

// read the current date and time from the CMOS clock
void rtc_read(rtc_time_t* t);

// format the current date & time as "DD-MM-YY--HH:MM:SS"
char* rtc_datetime(char* out, size_t size);

#endif /* SYS_CLOCK_H */

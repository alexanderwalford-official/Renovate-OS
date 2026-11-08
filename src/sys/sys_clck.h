/*
    SYS_CLCK_H
    This class file is responsible for handling all system time sensitive operations.

    - The PIT (programmable interval timer) fires IRQ0 1000 times a second, so "ticks" counts
      milliseconds since the timer started. Delays no longer depend on the speed of the processor.
    - The CMOS real time clock (RTC) gives the current date and time.

    The two clocks do very different jobs:

      PIT (Intel 8253/8254)
        A chip with three independent counters fed by a 1.193182MHz clock (an odd number that
        goes back to the original IBM PC, it is the 14.31818MHz TV colour crystal divided by 12).
        Each counter counts down from a "divisor" we choose and then fires again, so
        output frequency = 1193182 / divisor.
          channel 0  wired to IRQ0, our system timer (set up by clck_init())
          channel 1  used to refresh DRAM on ancient PCs, leave it alone
          channel 2  drives the PC speaker (see drivers/audio/tone.c)
        With divisor 1193 we actually get 1193182 / 1193 = about 1000.15Hz, so uptime runs
        roughly 13 seconds a day fast. That is fine for delays, the RTC is the real wall clock.

      CMOS RTC (Motorola MC146818 compatible)
        A battery backed clock that keeps counting while the computer is off. It lives behind
        I/O port 0x70 (choose a register) and 0x71 (read / write it). Depending on the BIOS it
        stores values in binary or BCD, and the hour in 12 or 24 hour format, so rtc_read() has
        to check and convert. It holds whatever time the BIOS was set to (often local time on
        Windows machines, UTC on Linux ones and in QEMU by default), there are no time zones here.

    https://wiki.osdev.org/Programmable_Interval_Timer
    https://wiki.osdev.org/CMOS
    https://wiki.osdev.org/RTC
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

// a calendar date and time, already converted to plain binary numbers in 24 hour format
typedef struct {
    uint8_t second; // 0-59
    uint8_t minute; // 0-59
    uint8_t hour;   // 0-23
    uint8_t day;    // day of the month, 1-31
    uint8_t month;  // 1-12
    uint16_t year;  // the full year, e.g. 2026
} rtc_time_t;

// one flag per thread: true = running / free, false = waiting inside clck_hang()
extern bool sys_threads_status[4]; // eventual support for quad threads, only state 0 is used currently
// true once clck_init() has programmed the PIT, sleep_ms() uses it to decide how to wait
extern bool clck_ready;

// start the system timer (requires idt_init() first)
// programs PIT channel 0 to 1000Hz and claims IRQ0. Ticks only start counting once interrupts
// are enabled with interrupts_enable()
void clck_init(void);

// check the timer really ticks (call once interrupts are on), returns false and switches every
// wait over to busy waiting if it doesn't, which happens when firmware has turned the PIT off
bool clck_verify(void);

// milliseconds since the timer started
// (0 before clck_init(), wraps back to 0 after about 49.7 days)
uint32_t clck_uptime_ms(void);

// wait for the given number of milliseconds
// uses the timer and "hlt" when it can, otherwise a rough busy wait (before clck_init() or with
// interrupts disabled, e.g. inside an IRQ handler). The wait can be up to 1ms shorter than asked
void sleep_ms(uint32_t ms);

// halts a thread for the desired time (milliseconds), we will later add a argument for thread ID and then will check if it is in use
// (for now it is always thread 0, and a time of 0 or less returns straight away)
void clck_hang(long time);

// read the current date and time from the CMOS clock
// fills in *t, handling BCD, 12 hour mode and a missing century register
void rtc_read(rtc_time_t* t);

// format the current date & time as "DD-MM-YY--HH:MM:SS"
// out needs room for at least 19 bytes (18 characters + the terminator), a smaller buffer is
// truncated safely. Returns out, so the call can be passed straight to printf
char* rtc_datetime(char* out, size_t size);

#endif /* SYS_CLOCK_H */

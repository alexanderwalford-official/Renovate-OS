/*
    sys_clck.c
    PIT system timer and CMOS real time clock, see sys_clck.h.
*/
#include "sys_clck.h"
#include "interrupts.h"
#include "../drivers/input/GIO.h"
#include "../lib/primary_definitions.h"

#define CMOS_ADDRESS  0x70
#define CMOS_DATA     0x71

bool sys_threads_status[4] = {true, false, false, false};
bool clck_ready = false;

static volatile uint32_t ticks = 0; // milliseconds since clck_init()

// IRQ0 handler, the end of interrupt is sent by isr_dispatch
static void timer_handler(struct regs* r) {
    (void)r;
    ticks++;
}

void clck_init(void) {
    uint16_t divisor = PIT_FREQUENCY / PIT_HZ;
    outb(PIT_COMMAND, 0x36); // channel 0, lobyte/hibyte, mode 3 (square wave)
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));

    irq_install_handler(0, timer_handler);
    pic_unmask(0);
    clck_ready = true;
}

uint32_t clck_uptime_ms(void) {
    return ticks;
}

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

void clck_hang(long time) {
    if (time <= 0) {
        return;
    }
    sys_threads_status[0] = false;
    sleep_ms((uint32_t)time);
    sys_threads_status[0] = true;
}

// the raw RTC registers, all single bytes so two reads can be compared with memcmp (no padding)
typedef struct {
    uint8_t second, minute, hour, day, month, year, century;
} rtc_raw_t;

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDRESS, reg);
    io_wait();
    return inb(CMOS_DATA);
}

static bool rtc_update_in_progress(void) {
    return (cmos_read(0x0A) & 0x80) != 0;
}

static void rtc_read_raw(rtc_raw_t* raw) {
    // wait (with a limit) for the RTC to finish any update so we do not read half old, half new values
    for (int i = 0; i < 100000 && rtc_update_in_progress(); i++) {
    }
    raw->second = cmos_read(0x00);
    raw->minute = cmos_read(0x02);
    raw->hour = cmos_read(0x04);
    raw->day = cmos_read(0x07);
    raw->month = cmos_read(0x08);
    raw->year = cmos_read(0x09);
    raw->century = cmos_read(0x32);
}

static uint8_t bcd_to_binary(uint8_t value) {
    return (uint8_t)((value & 0x0F) + ((value >> 4) * 10));
}

void rtc_read(rtc_time_t* t) {
    rtc_raw_t raw, last;

    // read until two reads in a row match, so the value cannot change half way through
    rtc_read_raw(&raw);
    for (int tries = 0; tries < 10; tries++) {
        last = raw;
        rtc_read_raw(&raw);
        if (memcmp(&last, &raw, sizeof(raw)) == 0) {
            break;
        }
    }

    uint8_t status_b = cmos_read(0x0B);
    bool pm = (raw.hour & 0x80) != 0;
    raw.hour &= 0x7F;

    // convert from BCD if needed
    if (!(status_b & 0x04)) {
        raw.second = bcd_to_binary(raw.second);
        raw.minute = bcd_to_binary(raw.minute);
        raw.hour = bcd_to_binary(raw.hour);
        raw.day = bcd_to_binary(raw.day);
        raw.month = bcd_to_binary(raw.month);
        raw.year = bcd_to_binary(raw.year);
        raw.century = bcd_to_binary(raw.century);
    }

    // convert 12 hour clock to 24 hour clock if needed
    if (!(status_b & 0x02)) {
        if (pm && raw.hour < 12) raw.hour = (uint8_t)(raw.hour + 12);
        if (!pm && raw.hour == 12) raw.hour = 0;
    }

    // the century register is not on every machine, assume 20xx without it
    uint8_t century = (raw.century >= 19 && raw.century <= 21) ? raw.century : 20;

    t->second = raw.second;
    t->minute = raw.minute;
    t->hour = raw.hour;
    t->day = raw.day;
    t->month = raw.month;
    t->year = (uint16_t)(century * 100 + raw.year);
}

char* rtc_datetime(char* out, size_t size) {
    rtc_time_t t;
    rtc_read(&t);
    snprintf(out, size, "%02u-%02u-%02u--%02u:%02u:%02u",
             t.day, t.month, t.year % 100, t.hour, t.minute, t.second);
    return out;
}

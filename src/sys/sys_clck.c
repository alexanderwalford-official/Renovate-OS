/*
    sys_clck.c
    PIT system timer and CMOS real time clock, see sys_clck.h.

    Two halves:

      the timer   clck_init() programs PIT channel 0 to interrupt 1000 times a second and hooks
                  IRQ0. timer_handler() adds one to "ticks" on every interrupt, so ticks is the
                  uptime in milliseconds. sleep_ms() and clck_hang() wait by watching ticks,
                  halting the CPU in between so an idle AerOS does not spin at 100%.

      the RTC     rtc_read() reads the battery backed CMOS clock. The awkward part is that the
                  clock updates itself once a second, can store numbers in BCD, can run in
                  12 hour mode and may or may not have a century register, so the code reads
                  carefully (wait for the update to finish, read twice until both agree) and
                  then converts everything to plain 24 hour binary values.

    CMOS registers used here (written to port 0x70, then read from port 0x71):
      0x00 seconds   0x02 minutes   0x04 hours   0x07 day of month   0x08 month
      0x09 year (last two digits)   0x32 century (common, but not guaranteed)
      0x0A status A, bit 7 = update in progress
      0x0B status B, bit 1 = 24 hour mode, bit 2 = binary mode (clear means BCD)
    (register 0x06, the day of the week, is famously unreliable so it is not used)

    https://wiki.osdev.org/Programmable_Interval_Timer
    https://wiki.osdev.org/CMOS
*/
#include "sys_clck.h"
#include "interrupts.h"
#include "../drivers/input/GIO.h"
#include "../lib/primary_definitions.h"
#include "../lib/crypto.h"

// CMOS index port and data port. Bit 7 of the byte written to 0x70 also switches off NMIs (non
// maskable interrupts), every register number we write is below 0x80 so NMIs stay enabled
#define CMOS_ADDRESS  0x70
#define CMOS_DATA     0x71

// thread 0 (the only one so far) starts out free, see sys_clck.h
bool sys_threads_status[4] = {true, false, false, false};
bool clck_ready = false;

// volatile because the IRQ0 handler changes it behind the compiler's back: without it the
// compiler could keep ticks in a register inside sleep_ms() and wait forever. A 32 bit aligned
// read or write is a single instruction on x86, so no locking is needed
static volatile uint32_t ticks = 0; // milliseconds since clck_init()

// IRQ0 handler, the end of interrupt is sent by isr_dispatch
// runs once a millisecond, so it does as little as possible
static void timer_handler(struct regs* r) {
    // the saved registers are not needed, this just silences the unused parameter warning
    (void)r;
    ticks++;
    // the exact cycle each tick lands on jitters a little, which helps the random generator
    crypto_add_entropy(ticks);
}

// program PIT channel 0 for PIT_HZ interrupts a second and hook IRQ0, see sys_clck.h
void clck_init(void) {
    // 1193182 / 1000 = 1193 (integer division), so the real rate is about 1000.15Hz.
    // The divisor is a 16 bit value, which is why the slowest possible rate is about 18.2Hz
    uint16_t divisor = PIT_FREQUENCY / PIT_HZ;
    // the command byte 0x36 = 00 11 011 0:
    //   bits 6-7  00   select channel 0
    //   bits 4-5  11   we will send the divisor as low byte then high byte
    //   bits 1-3  011  mode 3, square wave generator: the output flips every half period and
    //                  IRQ0 fires on each rising edge, i.e. once per divisor count
    //   bit 0     0    count in binary, not BCD
    outb(PIT_COMMAND, 0x36); // channel 0, lobyte/hibyte, mode 3 (square wave)
    // the two halves of the divisor, in the order the command byte promised
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));

    // install the handler before unmasking, so the first tick never finds an empty slot
    irq_install_handler(0, timer_handler);
    pic_unmask(0);
    clck_ready = true;
}

// milliseconds since the timer started, see sys_clck.h
// milliseconds counted by the busy wait in sleep_ms, used instead of ticks when the timer is dead
static uint32_t busy_ms = 0;

uint32_t clck_uptime_ms(void) {
    return clck_ready ? ticks : busy_ms;
}

// wait ms milliseconds, see sys_clck.h for when each method is used
void sleep_ms(uint32_t ms) {
    // "hlt" with interrupts disabled would sleep forever (no interrupt could wake the CPU),
    // so the timer path is only safe when the timer is running AND interrupts are on
    if (clck_ready && interrupts_enabled()) {
        uint32_t start = ticks;
        // unsigned subtraction still gives the right answer when ticks wraps past 0xFFFFFFFF.
        // Because the next tick can arrive at any moment after start was read, the real wait is
        // somewhere between ms - 1 and ms milliseconds
        while ((uint32_t)(ticks - start) < ms) {
            cpu_idle(); // sleep until the next interrupt instead of burning the CPU
        }
    } else {
        // fallback before the timer is running: each port write takes roughly 1 microsecond
        // (so 1000 of them is about a millisecond, very roughly, it depends on the machine / emulator)
        for (uint32_t i = 0; i < ms; i++) {
            for (int j = 0; j < 1000; j++) {
                io_wait();
            }
            busy_ms++;
        }
    }
}

// Some newer PCs let the firmware switch the old PIT chip off to save power, and then IRQ0 never
// comes. Everything that waits on ticks would wait forever, so once interrupts are on we give the
// timer about 50ms to tick. If it doesn't, clck_ready goes false and every wait becomes a busy wait.
bool clck_verify(void) {
    if (!clck_ready || !interrupts_enabled()) {
        return false;
    }
    uint32_t start = ticks;
    for (int i = 0; i < 50000 && ticks == start; i++) {
        io_wait();
    }
    if (ticks == start) {
        clck_ready = false;
    }
    return clck_ready;
}

// block thread 0 for time milliseconds, marking it busy while it waits, see sys_clck.h
void clck_hang(long time) {
    // nothing to wait for (and a negative time would turn into a huge unsigned number below)
    if (time <= 0) {
        return;
    }
    sys_threads_status[0] = false;
    sleep_ms((uint32_t)time);
    sys_threads_status[0] = true;
}

// the raw RTC registers, all single bytes so two reads can be compared with memcmp (no padding)
// (padding bytes would hold whatever was on the stack and make identical readings look different)
typedef struct {
    uint8_t second, minute, hour, day, month, year, century;
} rtc_raw_t;

// read CMOS register reg: select it on the index port, give the chip a moment, then read the data
// port. The selection and the read are two separate steps, so nothing else may touch the CMOS in
// between (nothing else in AerOS uses it, so no lock is needed yet)
static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDRESS, reg);
    io_wait();
    return inb(CMOS_DATA);
}

// status register A bit 7 ("UIP") is set while the RTC is updating its time registers, which it
// does once a second and which takes up to about 2ms. Values read during an update can be a mix
// of the old and the new time, e.g. 12:59:59 turning into 12:00:00 or 13:59:00
static bool rtc_update_in_progress(void) {
    return (cmos_read(0x0A) & 0x80) != 0;
}

// read every time register exactly as the chip stores them (maybe BCD, maybe 12 hour)
static void rtc_read_raw(rtc_raw_t* raw) {
    // wait (with a limit) for the RTC to finish any update so we do not read half old, half new values
    // the limit means a broken or missing RTC whose flag never clears cannot hang the boot.
    // The empty body is on purpose, all the work happens in the loop condition
    for (int i = 0; i < 100000 && rtc_update_in_progress(); i++) {
    }
    // an update could still begin part way through these reads, which is why rtc_read() reads
    // twice and compares
    raw->second = cmos_read(0x00);
    raw->minute = cmos_read(0x02);
    raw->hour = cmos_read(0x04);
    raw->day = cmos_read(0x07);
    raw->month = cmos_read(0x08);
    raw->year = cmos_read(0x09);
    raw->century = cmos_read(0x32);
}

// BCD (binary coded decimal) keeps one decimal digit in each 4 bit nibble, so the decimal number
// 59 is stored as 0x59. The answer is the low nibble plus ten times the high nibble:
// 0x59 -> 9 + 5 * 10 = 59
static uint8_t bcd_to_binary(uint8_t value) {
    return (uint8_t)((value & 0x0F) + ((value >> 4) * 10));
}

// read the date and time into *t, see sys_clck.h
void rtc_read(rtc_time_t* t) {
    rtc_raw_t raw, last;

    // read until two reads in a row match, so the value cannot change half way through
    // (normally the first comparison already matches, after 10 tries we just use the latest read)
    rtc_read_raw(&raw);
    for (int tries = 0; tries < 10; tries++) {
        last = raw;
        rtc_read_raw(&raw);
        if (memcmp(&last, &raw, sizeof(raw)) == 0) {
            break;
        }
    }

    // status register B says which formats the clock uses
    uint8_t status_b = cmos_read(0x0B);
    // in 12 hour mode bit 7 of the hour is the PM flag. It has to come off BEFORE the BCD
    // conversion, otherwise it would be treated as a digit. In 24 hour mode bit 7 is always 0,
    // so taking it off does no harm
    bool pm = (raw.hour & 0x80) != 0;
    raw.hour &= 0x7F;

    // convert from BCD if needed
    // (status B bit 2 set = the clock already stores binary numbers)
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
    // (status B bit 1 set = the clock is already in 24 hour mode). A 12 hour clock goes
    // 12, 1, 2 ... 11 AM then 12, 1 ... 11 PM, so 1-11 PM gain 12 hours, 12 AM (midnight)
    // becomes 0 and 12 PM (midday) stays 12
    if (!(status_b & 0x02)) {
        if (pm && raw.hour < 12) raw.hour = (uint8_t)(raw.hour + 12);
        if (!pm && raw.hour == 12) raw.hour = 0;
    }

    // the century register is not on every machine, assume 20xx without it
    // (the proper way to find it is the "century" field of the ACPI FADT table, which we do not
    // read yet. A missing register usually reads as 0 or junk, which the range check rejects)
    uint8_t century = (raw.century >= 19 && raw.century <= 21) ? raw.century : 20;

    t->second = raw.second;
    t->minute = raw.minute;
    t->hour = raw.hour;
    t->day = raw.day;
    t->month = raw.month;
    // e.g. century 20 and year 26 -> 2026
    t->year = (uint16_t)(century * 100 + raw.year);
}

// write the current date and time into out as "DD-MM-YY--HH:MM:SS" and return out, see sys_clck.h
char* rtc_datetime(char* out, size_t size) {
    rtc_time_t t;
    rtc_read(&t);
    // %02u pads with a leading zero, year % 100 keeps only the last two digits of the year
    snprintf(out, size, "%02u-%02u-%02u--%02u:%02u:%02u",
             t.day, t.month, t.year % 100, t.hour, t.minute, t.second);
    return out;
}

/*
    rt.c - RT mode's real-time tasks (see rt.h for what is promised, and what isn't)

    Every millisecond the timer interrupt calls rt_tick(), which goes through the tasks, most
    urgent (shortest period) first, and runs each one that is due:

        tick 1000:  heartbeat due (every 10ms)  -> run it, time it, due again at 1010
                    stress not due until 1003   -> skip

    The table is kept in that order by rt_add, so "most urgent first" is just "from the top".
*/

#include "rt.h"
#include "sys_clck.h"

static bool on = false;
static rt_task_t tasks[RT_TASKS_MAX];
static int task_count = 0;
static uint32_t cycles_per_us = 1;   // measured by rt_start

static uint32_t cycles (void) {
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    (void)high; // the low 32 bits are plenty for timing one run (seconds, even at 4 GHz)
    return low;
}

bool rt_on (void) {
    return on;
}

// The rate monotonic bound, n * (2^(1/n) - 1), in tenths of a percent for 1 to 8 tasks
// (worked out beforehand: the kernel has no floating point)
uint32_t rt_limit (int count) {
    static const uint32_t limits[RT_TASKS_MAX] = { 1000, 828, 779, 756, 743, 734, 728, 724 };
    return count >= 1 && count <= RT_TASKS_MAX ? limits[count - 1] : 0;
}

// the share of the processor one task needs, in tenths of a percent: budget / period
static uint32_t share (uint32_t period_ms, uint32_t budget_us) {
    return budget_us / period_ms;   // (budget_us / 1000) / period_ms, times 1000
}

uint32_t rt_load (void) {
    uint32_t total = 0;
    for (int i = 0; i < task_count; i++) {
        if (!tasks[i].stopped) {
            total += share(tasks[i].period_ms, tasks[i].budget_us);
        }
    }
    return total;
}

int rt_add (const char* name, uint32_t period_ms, uint32_t budget_us, void (*run)(void)) {
    if (!on) {
        return RT_ERR_OFF;
    }
    if (task_count == RT_TASKS_MAX) {
        return RT_ERR_FULL;
    }
    // would all the tasks, this one too, still always meet their deadlines?
    if (period_ms == 0 || rt_load() + share(period_ms, budget_us) > rt_limit(task_count + 1)) {
        return RT_ERR_TOO_MUCH;
    }

    // in its place by period, shortest first, so rt_tick runs the most urgent first. The timer
    // interrupt is held off while the table moves, so it never sees it half changed
    __asm__ volatile ("cli");
    int at = task_count;
    while (at > 0 && tasks[at - 1].period_ms > period_ms) {
        tasks[at] = tasks[at - 1];
        at--;
    }
    tasks[at] = (rt_task_t){ name, period_ms, budget_us, run, clkuptms() + period_ms, 0, 0, 0, false };
    task_count++;
    __asm__ volatile ("sti");
    return RT_OK;
}

void rt_tick (void) {
    if (!on) {
        return;
    }
    uint32_t now = clkuptms();
    for (int i = 0; i < task_count; i++) {
        rt_task_t* task = &tasks[i];
        // (now - due) as a signed number: >= 0 once the due time has come, even when the
        // millisecond count wraps round after 49 days
        if (task->stopped || (int32_t)(now - task->due) < 0) {
            continue;
        }
        uint32_t start = cycles();
        task->run();
        uint32_t took_us = (cycles() - start) / cycles_per_us;

        task->runs++;
        task->due += task->period_ms;
        if (took_us > task->worst_us) {
            task->worst_us = took_us;
        }
        // the watchdog: too long a run is an overrun, and enough of them stop the task
        if (took_us > task->budget_us && ++task->overruns >= RT_OVERRUNS_MAX) {
            task->stopped = true;
        }
    }
}

int rt_count (void) {
    return task_count;
}

const rt_task_t* rt_task (int index) {
    return index >= 0 && index < task_count ? &tasks[index] : NULL;
}

// ---- the tasks that come with RT mode --------------------------------------------------------------

static volatile uint32_t heartbeats = 0;

// heartbeat: just counts. After t seconds it should have counted 100 * t, which shows the
// 10ms promise being kept, whatever else AerOS was busy with
static void heartbeat (void) {
    heartbeats++;
}

uint32_t rt_heartbeats (void) {
    return heartbeats;
}

// stress: busy for 500 microseconds, against a budget of 100, so every run is an overrun and
// the watchdog stops it after three
static void stress (void) {
    uint32_t start = cycles();
    while ((cycles() - start) / cycles_per_us < 500) {
    }
}

int rt_add_stress (void) {
    return rt_add("stress", 5, 100, stress);
}

void rt_start (void) {
    // how fast the processor counts cycles: count them over 20ms of the timer
    uint32_t before = cycles();
    sleep_ms(20);
    uint32_t per_us = (cycles() - before) / 20000;
    cycles_per_us = per_us > 0 ? per_us : 1;

    on = true;
    rt_add("heartbeat", 10, 50, heartbeat);
}

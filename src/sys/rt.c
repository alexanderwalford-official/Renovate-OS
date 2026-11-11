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

// the processor's cycle counter, which counts up once per clock cycle. "rdtsc" puts the 64 bit
// count in edx:eax, and only eax is kept. A run that crosses the moment the low half wraps still
// times correctly, because the subtraction in the callers wraps the same way
static uint32_t cycles (void) {
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    (void)high; // the low 32 bits are plenty for timing one run (seconds, even at 4 GHz)
    return low;
}

// whether RT mode has been started (rt_start)
bool rt_on (void) {
    return on;
}

// The rate monotonic bound, n * (2^(1/n) - 1), in tenths of a percent for 1 to 8 tasks
// (worked out beforehand: the kernel has no floating point).
// Liu and Layland showed that when the shortest period always runs first, the tasks are sure
// to meet every deadline as long as their shares added up stay under this. One task can use
// all of the processor (1000), two 82.8%, and it heads down towards 69.3% for many tasks.
// It is a safe limit, not an exact one: some sets above it would still be fine
uint32_t rt_limit (int count) {
    static const uint32_t limits[RT_TASKS_MAX] = { 1000, 828, 779, 756, 743, 734, 728, 724 };
    return count >= 1 && count <= RT_TASKS_MAX ? limits[count - 1] : 0;
}

// the share of the processor one task needs, in tenths of a percent: budget / period.
// e.g. 50us every 10ms = 5 tenths, 0.5%. Whole numbers round down, so a tiny task counts as 0
static uint32_t share (uint32_t period_ms, uint32_t budget_us) {
    return budget_us / period_ms;   // (budget_us / 1000) / period_ms, times 1000
}

// the shares of all the tasks still running, added up (stopped tasks no longer use any time)
uint32_t rt_load (void) {
    uint32_t total = 0;
    for (int i = 0; i < task_count; i++) {
        if (!tasks[i].stopped) {
            total += share(tasks[i].period_ms, tasks[i].budget_us);
        }
    }
    return total;
}

// add a real-time task, if it fits within the rate monotonic bound, see rt.h
int rt_add (const char* name, uint32_t period_ms, uint32_t budget_us, void (*run)(void)) {
    if (!on) {
        return RT_ERR_OFF;
    }
    if (task_count == RT_TASKS_MAX) {
        return RT_ERR_FULL;
    }
    // would all the tasks, this one too, still always meet their deadlines?
    // (a period of 0 would divide by zero in share, and mean "run all the time")
    if (period_ms == 0 || rt_load() + share(period_ms, budget_us) > rt_limit(task_count + 1)) {
        return RT_ERR_TOO_MUCH;
    }

    // in its place by period, shortest first, so rt_tick runs the most urgent first. The timer
    // interrupt is held off while the table moves, so it never sees it half changed
    __asm__ volatile ("cli");
    // insertion: start at the end and move each longer period task down one, until the place
    // for this one is found. Equal periods keep the order they were added in
    int at = task_count;
    while (at > 0 && tasks[at - 1].period_ms > period_ms) {
        tasks[at] = tasks[at - 1];
        at--;
    }
    // first due one period from now, with no runs, overruns or worst time yet, and not stopped
    tasks[at] = (rt_task_t){ name, period_ms, budget_us, run, clkuptms() + period_ms, 0, 0, 0, false };
    task_count++;
    __asm__ volatile ("sti");
    return RT_OK;
}

// run every task that is due, most urgent first. Called by the timer interrupt every millisecond
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
        // run it, timing it in cycles and turning that into microseconds
        uint32_t start = cycles();
        task->run();
        uint32_t took_us = (cycles() - start) / cycles_per_us;

        task->runs++;
        // due again one period after it was meant to run (not after now), so a late run doesn't
        // push every later run back as well
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

// how many tasks there are, stopped ones included
int rt_count (void) {
    return task_count;
}

// one task, by its place in the table (shortest period first), NULL past the end
const rt_task_t* rt_task (int index) {
    return index >= 0 && index < task_count ? &tasks[index] : NULL;
}

// ---- the tasks that come with RT mode --------------------------------------------------------------

// volatile: the timer interrupt counts it up while the program reads it
static volatile uint32_t heartbeats = 0;

// heartbeat: just counts. After t seconds it should have counted 100 * t, which shows the
// 10ms promise being kept, whatever else AerOS was busy with
static void heartbeat (void) {
    heartbeats++;
}

// how many times the heartbeat task has run
uint32_t rt_heartbeats (void) {
    return heartbeats;
}

// stress: busy for 500 microseconds, against a budget of 100, so every run is an overrun and
// the watchdog stops it after three
static void stress (void) {
    // spin, watching the cycle counter, until 500 microseconds have gone by
    uint32_t start = cycles();
    while ((cycles() - start) / cycles_per_us < 500) {
    }
}

// add the stress task: every 5ms with a 100us budget, so it passes the bound check (2%) but
// then breaks its promise, to show the watchdog at work
int rt_add_stress (void) {
    return rt_add("stress", 5, 100, stress);
}

// turn RT mode on and add the heartbeat task (called by init_boot when RT mode was picked)
void rt_start (void) {
    // how fast the processor counts cycles: count them over 20ms of the timer
    // (20ms = 20000us; 0 can only happen on a very odd machine, and would divide by zero later)
    uint32_t before = cycles();
    sleep_ms(20);
    uint32_t per_us = (cycles() - before) / 20000;
    cycles_per_us = per_us > 0 ? per_us : 1;

    // on before rt_add, which refuses tasks while RT mode is off.
    // heartbeat: every 10ms, allowed 50us
    on = true;
    rt_add("heartbeat", 10, 50, heartbeat);
}

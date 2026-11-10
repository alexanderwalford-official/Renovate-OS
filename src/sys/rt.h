/*
    rt.h - RT mode: a small real-time layer (experimental, picked in the boot options)

    Most of AerOS is "best effort": a program runs until it is done, and nothing promises how
    long anything takes. RT mode adds a few tasks that DO get a promise: each one runs every
    `period` milliseconds, from the timer interrupt, whatever the rest of AerOS is doing (a disk
    read, a busy AerLang loop...). Pick it with X on the splash screen, then [6].

        rt              the tasks: how often they run, their time budget, the longest they took
        rt stress       add a task that breaks its budget on purpose, to see the watchdog stop it

    THE PROMISE, AND HOW IT IS CHECKED

    Every task says how often it runs (its period) and the most time one run may take (its
    budget). Before a task is added, rt_add checks that all the tasks together can always meet
    their deadlines, with the classic test for "rate monotonic" scheduling (shorter period =
    more urgent): the share of the processor they need, adding up budget / period, must stay
    under n * (2^(1/n) - 1) for n tasks (100% for one, 82.8% for two... 72.4% for eight).
    A set of tasks that passes can never miss a deadline. One that doesn't is refused.
    https://en.wikipedia.org/wiki/Rate-monotonic_scheduling

    Every run is timed with the processor's cycle counter. A run that takes longer than its
    budget is an overrun, and the watchdog stops a task after three of them, so one bad task
    can't spoil the others' promise.

    WHAT IS NOT PROMISED (why it is "experimental")

    - The tasks run inside the timer interrupt, one after another, most urgent first, and each
      runs to the end: nothing interrupts a task. So a task that never returns hangs AerOS: the
      watchdog can only stop a task after a run, not during one.
    - Only these tasks are real-time. Programs, the disk, the network and memory allocation are
      exactly as they always are. Tasks are C functions in the kernel, as AerLang's timing can't
      be bounded, and they must not allocate memory or wait for anything.
    - In an emulator, time is only as steady as the computer the emulator runs on.
*/
#ifndef RT_H
#define RT_H

#include <stdint.h>
#include <stdbool.h>

#define RT_TASKS_MAX    8
#define RT_OVERRUNS_MAX 3   // overruns before the watchdog stops a task

// results of rt_add
#define RT_OK            0
#define RT_ERR_FULL     -1   // there are RT_TASKS_MAX tasks already
#define RT_ERR_TOO_MUCH -2   // with it, the tasks couldn't all meet their deadlines
#define RT_ERR_OFF      -3   // RT mode wasn't picked at boot

typedef struct {
    const char* name;
    uint32_t period_ms;     // it runs every this many milliseconds
    uint32_t budget_us;     // and one run may take at most this many microseconds
    void (*run)(void);
    uint32_t due;           // the timer tick (millisecond) of its next run
    uint32_t runs;
    uint32_t worst_us;      // the longest a run has taken
    uint32_t overruns;      // runs that took longer than the budget
    bool stopped;           // stopped by the watchdog
} rt_task_t;

// is RT mode on? (rt_start was called, at boot)
bool rt_on (void);

// switch RT mode on: measure the processor's speed (for timing the tasks) and add the
// heartbeat task, a 10ms tick. Called by init_boot when RT mode was picked
void rt_start (void);

// add a task: RT_OK, or an RT_ERR_ code
int rt_add (const char* name, uint32_t period_ms, uint32_t budget_us, void (*run)(void));

// run the tasks that are due. Called by the timer interrupt every millisecond
void rt_tick (void);

// the tasks, most urgent first (for RT), and the share of the processor they need and may
// have, in tenths of a percent
int rt_count (void);
const rt_task_t* rt_task (int index);
uint32_t rt_load (void);
uint32_t rt_limit (int task_count);

// how many beats the heartbeat task has counted: one every 10ms, so it shows the promise kept
uint32_t rt_heartbeats (void);

// add the "stress" task, which overruns its budget on purpose (RT STRESS)
int rt_add_stress (void);

#endif /* RT_H */

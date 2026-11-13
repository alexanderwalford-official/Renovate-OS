# RT mode (experimental)

Most of AerOS makes no promise about time: a program runs until it is done, and a disk read
takes as long as it takes. RT mode adds a few **real-time tasks** that do get a promise: each
runs every so many milliseconds, on time, whatever else AerOS is busy with.

To try it, press **X** on the splash screen as AerOS starts, then **6**. Everything else works
as in a normal boot.

## Seeing the tasks

```
rt             the tasks: how often they run, their budget, the longest they have taken
rt stress      add a task that breaks its budget on purpose
```

RT mode starts with one task, **heartbeat**, which counts every 10 milliseconds. Run a busy
AerLang loop, then `rt`: the count kept going at 100 a second the whole time.

## The promise, and how it is kept

Every task has a **period** (it runs every this many milliseconds) and a **budget** (the
longest one run may take). Before a task is added, AerOS checks that all the tasks together
can always meet their deadlines, and refuses one that would break that. `rt` shows how much of
the processor the tasks need, and how much is safe: 100% for one task, 82.8% for two, down to
72.4% for eight. The most urgent task (the shortest period) always runs first.

Every run is timed. A run longer than its budget is an **overrun**, and after three of them
the **watchdog** stops the task, so one bad task can't spoil the others. `rt stress` adds a
task that overruns every time, to watch that happen.

## Why it is experimental

- The tasks run inside the timer interrupt, one after another, and nothing can interrupt a
  task while it runs. So a task that never finishes would hang AerOS: the watchdog can only
  stop a task after a run, not during one.
- Only the tasks are real-time. Programs, files, the network and memory work as always.
- Tasks are written in C, inside AerOS, not in AerLang.
- In QEMU or VirtualBox, time is only as steady as the computer they run on.

How it works, and how to add a task of your own: `src/sys/rt.h`, or `src rt_tick`.

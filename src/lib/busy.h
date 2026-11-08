/*
    busy.h
    A hook for showing that something is still happening during long work.

    Code that can take a while (deriving a key from a password, decoding an MP3) calls busy()
    every so often. Normally that does nothing at all. When the terminal wants to show a
    spinner, it points busy_hook at a function that turns it (sys/ui.c, ui_busy_begin), and the
    long job animates it without knowing anything about the screen.

    AerOS runs one thing at a time, so nothing can animate while a job is busy unless the job
    itself lends a hand like this ("cooperative" updates). The hook keeps libraries like the
    crypto code free of any screen code.
*/
#ifndef BUSY_H
#define BUSY_H

// set by whoever is showing a spinner, NULL the rest of the time
extern void (*busy_hook)(void);

// "still working": cheap enough to call in a tight loop (the spinner itself only redraws a
// few times a second)
static inline void busy (void) {
    if (busy_hook) busy_hook();
}

#endif /* BUSY_H */

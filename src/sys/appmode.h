/*
    appmode.h - app mode: a program gets the memory of the parts of AerOS it doesn't use

    Before an AerLang program runs, AerOS has read all of it (the whole file is turned into
    tokens and a syntax tree first). So it knows every part of AerOS the program can possibly
    use: a built-in function can only be reached by its name, and every name is in the source.
    AerLang has no eval, and cmd() only works at the prompt, so there is no way round that.

    From those names AerOS works out the program's profile ("it uses graphics and files"), and
    puts away what the program will never touch, so its memory is free for the program:

        part        what it holds while it isn't in use      when app mode puts it away
        network     the card's buffers, about 84 KB          the program uses no nsend / nrecv

    Everything else is only in memory while it is being used already: the MP3 and video
    decoders, a sound's samples, the graphics back buffer. So there is nothing more to put away
    yet, but anything added to AerOS that keeps memory while idle belongs in the table above.

    Nothing is brought back when the program ends: a part that was put away starts again the
    next time anything uses it (the network, the first time you type NET or MSG).

    It happens by itself for every program (lang/aer_interp.c calls appmode_begin), however new:
    a file saved a second ago is read, and profiled, when it runs. A program can call appmode()
    to see its profile and how much memory was freed for it, and to put the parts away again
    (if something brought one back).
*/
#ifndef APPMODE_H
#define APPMODE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// the parts of AerOS a program can use (screen, keyboard and the language itself always are)
#define APP_GRAPHICS  (1u << 0)   // gopen, rect, image...
#define APP_SOUND     (1u << 1)   // play, sload, splay...
#define APP_VIDEO     (1u << 2)   // video
#define APP_FILES     (1u << 3)   // rdf, wrf, la...
#define APP_NETWORK   (1u << 4)   // nsend, nrecv
#define APP_PARTS     5

// the name of one part ("graphics"), for appmode() and MEM
const char* appmode_part_name (uint32_t part);

// the profile as words, "graphics, files", or "only the screen and keyboard"
void appmode_describe (uint32_t uses, char* out, size_t size);

// what app mode did for the program that is running (or ran last)
typedef struct {
    bool ran;           // false until the first appmode_begin
    uint32_t uses;      // its profile: APP_ bits
    size_t freed;       // bytes of memory given back to the heap for it
    size_t free;        // bytes the heap had free afterwards
} appmode_t;

// a program that uses only the parts in `uses` is starting: put the others away
void appmode_begin (uint32_t uses);

// put away again whatever the running program doesn't use and has started since (appmode()):
// what it gives back is added to what appmode_begin did
void appmode_again (void);

// what the last appmode_begin did
const appmode_t* appmode_now (void);

#endif /* APPMODE_H */

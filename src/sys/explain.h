/*
    explain.h - AerOS explains itself: WHY and SRC

    When something goes wrong, most systems print one line and leave you to search the web for
    what it means. AerOS remembers the last problem it reported, and WHY explains it:

        guest@0//: mkf /src/x.txt hi
        Could not create /src/x.txt: it can be read but not changed  (WHY explains)
        guest@0//: why
        ...what happened, why AerOS works that way, what you can do about it, and which C
        function made the decision...

    WHY with a name explains that instead: a command (WHY DEL), an AerLang function (WHY GOPEN)
    or any C function in AerOS (WHY FS_LIST). Those explanations are read out of AerOS's own
    source code (the /src/ folder, see source.h), from the comment written above each
    function, so they can't go out of date. SRC shows the code itself.

    SAYING SOMETHING WENT WRONG (so WHY can explain it)

        explain_say(PROBLEM_UNKNOWN_COMMAND, word, "Unrecognised command: %s", word);
        explain_say_fs(result, file, "Could not create %s", file);   // adds ": no space left"

    Both print the message like prtf does (the first few times after logging in with a reminder
    that WHY can explain it), then remember it. `subject` is the file or word the problem was
    about, or NULL: when a file isn't found, WHY looks for names like it.

    An AerLang program's errors are remembered by aer_error() (lang/aer_interp.c), through
    explain_program(), together with the lines of code around the error.
*/
#ifndef EXPLAIN_H
#define EXPLAIN_H

#include <stdbool.h>

// the kinds of problem WHY knows how to explain (the explanations are in explain.c)
typedef enum {
    PROBLEM_NONE,             // nothing has gone wrong since logging in

    // the file system's FS_ERR_ codes, in the same order (explain_say_fs picks one)
    PROBLEM_NOT_FOUND,
    PROBLEM_EXISTS,
    PROBLEM_BAD_NAME,
    PROBLEM_FULL,
    PROBLEM_NOT_EMPTY,
    PROBLEM_DISK_ERROR,
    PROBLEM_IN_USE,           // deleting the folder you are in
    PROBLEM_NO_MEMORY,
    PROBLEM_READ_ONLY,        // changing /src/

    // everything else
    PROBLEM_UNKNOWN_COMMAND,
    PROBLEM_ENCRYPTED,        // changing an encrypted file
    PROBLEM_WRONG_PASSWORD,
    PROBLEM_NO_SCREEN,        // pictures or video on the text screen
    PROBLEM_NO_SOUND_CARD,
    PROBLEM_NOT_PLAYABLE,     // PLY on a file it can't play
    PROBLEM_NO_VERSIONS,      // VERS or UNDO on a file with no old versions
    PROBLEM_PROGRAM,          // an AerLang program stopped with an error
    PROBLEM_TYPED_CODE,       // AerLang typed at the prompt stopped with an error
    PROBLEM_OTHER,            // something WHY has no explanation for (yet)

    PROBLEM_COUNT
} problem_t;

// print a problem like prtf (a newline is added), and remember it for WHY.
// subject: the file or word it was about, NULL if none
void explain_say (problem_t problem, const char* subject, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

// the same for a file system result code (FS_ERR_...): prints the message, then ": " and what
// the code means ("Could not create notes.txt: no space left")
void explain_say_fs (int fs_error, const char* subject, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

// which problem a result code from the encryption (FCRYPT_ERR_), sound (SOUND_ERR_) or video
// (VIDEO_ERR_) code is
problem_t explain_fcrypt_problem (int fcrypt_error);
problem_t explain_sound_problem (int sound_error);
problem_t explain_video_problem (int video_error);

// remember that an AerLang program stopped: its name (NULL for code typed at the prompt), the
// line (0 if not known), the error message, and its source code (NULL if there is none worth
// showing). Only remembers: aer_error has already printed the message
void explain_program (const char* program, int line, const char* message, const char* source);

// WHY on its own: explain the last problem
void explain_last (void);

// forget the last problem (logging out: the next person shouldn't see it)
void explain_forget (void);

// WHY <name>, for a C function: where it is, what the comment above it says, and the AerOS
// functions it uses, each with the first line of its own comment. False if there's no such
// function
bool explain_function (const char* function);

// SRC <name>, for a C function: print it with its comment and line numbers. False if there's
// no such function
bool explain_show_code (const char* function);

#endif /* EXPLAIN_H */

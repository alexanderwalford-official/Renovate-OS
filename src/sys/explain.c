/*
    explain.c - WHY and SRC: AerOS explains itself (see explain.h)

    Three parts:

    1. THE EXPLANATIONS. One for each kind of problem (problem_t), written out in full in the
       table below: what happened, why AerOS works that way, what you can do about it, and the
       C function that makes the decision, so you can read it for yourself with SRC.

       Commands are written as "@" and the function that runs them: "@cli_list shows what is
       here" prints as "LA shows what is here". The name comes from the command table
       (cli_command_called, sys/terminal/t_int.c), so renaming a command there renames it in
       these explanations too.

    2. THE LAST PROBLEM. explain_say() and the functions like it print a problem and keep a
       copy of it here, for WHY to explain later.

    3. READING THE SOURCE. WHY <function> and SRC <function> find the function in /src/ (the
       source code built into AerOS, sys/source.c), and print the comment above it and what it
       uses, or the code itself.
*/

#include "explain.h"
#include "source.h"
#include "f_sys.h"
#include "fcrypt.h"
#include "sound.h"
#include "video.h"
#include "terminal/t_int.h"
#include "../lib/primary_definitions.h"

#define WIDTH 78     // where text is wrapped: the 80 column screen, less a little margin
#define INDENT 2     // explanations are indented this much under their headings

// the first few problems after logging in end with a reminder that WHY can explain them.
// After that you know, and it would just be noise
#define HINTS 3


// =================================================================================================
//  1. The explanations
// =================================================================================================

typedef struct {
    const char* what;       // what happened, in a sentence
    const char* why;        // why AerOS works like this
    const char* fix;        // what you can do about it
    const char* code;       // the C function that makes the decision (SRC shows it), or NULL
    const char* code_vfs;   // ...if it's a different one without a disk (VFS mode), or NULL
} explanation_t;

static const explanation_t explanations[PROBLEM_COUNT] = {
    [PROBLEM_NOT_FOUND] = {
        "AerOS looked, and there is nothing with that name there.",
        "A name is looked for in the folder you are in (the prompt shows which), unless it "
        "starts with a /: then it is looked for from the top folder down. Upper and lower case "
        "don't matter.",
        "@cli_list shows what is here. Type the first few letters of a name and press Tab: "
        "AerOS finishes it for you, so it can't be mistyped.",
        "fs_split_path", NULL,
    },
    [PROBLEM_EXISTS] = {
        "Something with that name is already there.",
        "Making a file or folder never replaces one that is already there, so nothing is ever "
        "overwritten by accident.",
        "Pick another name. Or change the file that is there: @cli_write_file adds a line to "
        "it, @cli_edit opens it in the editor, and @cli_delete deletes it.",
        "fat_create_file", "mem_create_file",
    },
    [PROBLEM_BAD_NAME] = {
        "AerOS can't use that name.",
        "No name can have a / in it (that is what separates folders), or be . or .. (they mean "
        "\"this folder\" and \"the one above\"). On a disk, names are also saved in the 8.3 "
        "form that every FAT disk understands: up to 8 letters or digits, a dot, then up to 3 "
        "more, like NOTES.TXT.",
        "Use a shorter name, like notes.txt or todo.aer.",
        "fat_make_83", "fs_valid_name",
    },
    [PROBLEM_FULL] = {
        "There is no room left for it.",
        "On a disk, every cluster (the blocks files are kept in) is in use, or the top folder of "
        "a FAT16 disk, which has a fixed number of places, is full. Without a disk, files are "
        "kept in memory in fixed size tables: 64 files and 32 folders at most.",
        "@cli_delete what you don't need any more. @cli_memory shows how full things are.",
        "fat_alloc_cluster", "mem_create_file",
    },
    [PROBLEM_NOT_EMPTY] = {
        "The folder still has something in it.",
        "A folder is only removed once it is empty, so nothing inside it is lost without you "
        "knowing.",
        "@cli_delete the folder: it says how many files are inside, and asks before emptying it.",
        "fat_delete_dir", "mem_delete_folder",
    },
    [PROBLEM_DISK_ERROR] = {
        "The disk said it couldn't read or write.",
        "AerOS talks to IDE drives itself (drivers/storage/ata.c). The drive answered with its "
        "error bit set, or didn't answer at all.",
        "Try again. @cli_drives lists the drives AerOS can see. In QEMU, check that no other "
        "program has the disk image open.",
        "drive_read", NULL,
    },
    [PROBLEM_IN_USE] = {
        "That is the folder you are in, or one it is inside.",
        "Deleting it would leave you standing in a folder that doesn't exist any more.",
        "@cli_set_directory somewhere else first (@cli_set_directory / goes to the top), then "
        "delete it.",
        "fs_delete_d", NULL,
    },
    [PROBLEM_NO_MEMORY] = {
        "AerOS ran out of memory.",
        "Everything shares one heap: a 4MB pool, plus the free memory found when the computer "
        "started. Files are read into memory whole, so a big one needs that much memory free.",
        "@cli_memory shows how much is in use. In QEMU, give the computer more memory: -m 256.",
        "malloc", NULL,
    },
    [PROBLEM_READ_ONLY] = {
        "That is in /src/, AerOS's own source code, which can be read but not changed.",
        "The files in /src/ are the ones this copy of AerOS was built from, built into AerOS "
        "itself, so they always match the code that is running. They aren't on a disk, so "
        "there is nowhere a change could be saved.",
        "Read anything in there with @cli_read_file. To try out a change, copy the file into "
        "your own folder first, e.g. in AerLang: wrf(\"fat.c\", rdf(\"/src/sys/fat.c\"))",
        "fs_create_bytes", NULL,
    },
    [PROBLEM_UNKNOWN_COMMAND] = {
        "That isn't a command, or an app AerOS could find.",
        "The first word on a line is looked up in the command table (@cli_help lists it). If "
        "it isn't there, AerOS looks for an AerLang app called that: hello runs "
        "/bin/hello.aer, and a .aer file in the folder you are in runs too. Anything else "
        "that looks like code (let x = 1, 2 + 2, print(x)) runs as AerLang, but a word "
        "AerLang doesn't know, on its own or followed by more words, is taken for a mistyped "
        "command.",
        "@cli_help lists the commands. Type a letter or two and press Tab to see what fits.",
        "HandleInput", NULL,
    },
    [PROBLEM_ENCRYPTED] = {
        "The file is encrypted, so it can't be changed as it is.",
        "An encrypted file holds scrambled bytes, and a tag that proves nobody has changed "
        "them. Changing the scrambled bytes would break it for good, even with the password.",
        "@cli_decrypt it, change it, then @cli_encrypt it again. @cli_read_file shows what is "
        "inside without unlocking it.",
        "refuse_if_encrypted", NULL,
    },
    [PROBLEM_WRONG_PASSWORD] = {
        "That isn't the password the file was locked with (or the file has been changed).",
        "The password is turned into the key slowly on purpose (PBKDF2, thousands of rounds), "
        "so trying password after password takes ages. The file's tag only matches with the "
        "right key, so AerOS can tell before it shows anything.",
        "Check Caps Lock and try again. There is no way in without the password: that is the "
        "point of it.",
        "fcrypt_read_file", NULL,
    },
    [PROBLEM_NO_SCREEN] = {
        "That needs a graphics screen, and AerOS is using the text screen.",
        "Pictures and video need a framebuffer (memory where every pixel can be set), and GRUB "
        "sets one up when AerOS boots from its ISO. Started straight from main.elf (QEMU's "
        "-kernel), there is only the 80 x 25 text screen.",
        "Boot from the ISO (make run). @cli_resolution shows the screen mode.",
        "gfx_begin", NULL,
    },
    [PROBLEM_NO_SOUND_CARD] = {
        "There is no sound card AerOS can use.",
        "AerOS has a driver for the AC'97 sound card (drivers/audio/ac97.c), which QEMU and "
        "VirtualBox can both pretend to have. It is looked for on the PCI bus as AerOS starts.",
        "In QEMU, add the card: -device AC97 (make run does). @cli_beep still works, it uses "
        "the PC speaker instead.",
        "ac97_init", NULL,
    },
    [PROBLEM_NOT_PLAYABLE] = {
        "@cli_play can't play that file.",
        "@cli_play plays WAV files (uncompressed sound), MP3 files and MPEG-1 videos (.mpg). It "
        "goes by what is inside the file, not by its name.",
        "Convert it on another computer, e.g. for a video: ffmpeg -i in.mp4 -c:v mpeg1video "
        "-c:a mp2 out.mpg. @cli_docs VIDEO and @cli_docs SOUND say more.",
        "cli_play", NULL,
    },
    [PROBLEM_NO_VERSIONS] = {
        "AerOS hasn't kept any old versions of that file.",
        "Files in home folders (/home/<name>/) remember their past: before one is changed or "
        "deleted, a copy goes into the home folder's versions folder, and the last 10 are "
        "kept. Files anywhere else don't, and neither do files over 1MB, more than one copy "
        "every 10 seconds, or the files of a folder deleted in one go. A new file has none "
        "until it is changed.",
        "@cli_versions on its own lists the files in your home folder that have versions.",
        "versions_keep", NULL,
    },
    [PROBLEM_PROGRAM] = {
        NULL, // the error message itself is shown instead
        "AerLang stops a program at the first thing it can't do, and says which line it was on.",
        "@cli_edit the program to fix it (Ctrl+R in the editor runs it again). @cli_docs "
        "AERLANG explains the language.",
        NULL, NULL,
    },
    [PROBLEM_TYPED_CODE] = {
        NULL, // the error message itself is shown instead
        "Whatever isn't a command or an app is run as AerLang, and AerLang stops at the first "
        "thing it can't do.",
        "Press the up arrow to bring the line back, change it, and press Enter. @cli_docs "
        "AERLANG explains the language.",
        NULL, NULL,
    },
    [PROBLEM_OTHER] = {
        "WHY has no explanation for that problem yet.",
        "The message is all AerOS knows about it.",
        "@cli_src followed by the command's name shows the code that printed the message.",
        NULL, NULL,
    },
};

problem_t explain_fcrypt_problem (int fcrypt_error) {
    switch (fcrypt_error) {
        case FCRYPT_ERR_NOT_FOUND: return PROBLEM_NOT_FOUND;
        case FCRYPT_ERR_PASSWORD:  return PROBLEM_WRONG_PASSWORD;
        case FCRYPT_ERR_NO_MEMORY: return PROBLEM_NO_MEMORY;
        default:                   return PROBLEM_OTHER;
    }
}

problem_t explain_sound_problem (int sound_error) {
    switch (sound_error) {
        case SOUND_ERR_NO_CARD:   return PROBLEM_NO_SOUND_CARD;
        case SOUND_ERR_NOT_FOUND: return PROBLEM_NOT_FOUND;
        case SOUND_ERR_FORMAT:    return PROBLEM_NOT_PLAYABLE;
        case SOUND_ERR_NO_MEMORY: return PROBLEM_NO_MEMORY;
        default:                  return PROBLEM_OTHER;
    }
}

problem_t explain_video_problem (int video_error) {
    switch (video_error) {
        case VIDEO_ERR_NOT_FOUND: return PROBLEM_NOT_FOUND;
        case VIDEO_ERR_FORMAT:    return PROBLEM_NOT_PLAYABLE;
        case VIDEO_ERR_NO_SCREEN: return PROBLEM_NO_SCREEN;
        case VIDEO_ERR_NO_MEMORY: return PROBLEM_NO_MEMORY;
        default:                  return PROBLEM_OTHER;
    }
}

// the FS_ERR_ codes run from -1 down, in the same order as the PROBLEM_ ones from
// PROBLEM_NOT_FOUND up, so the code works out which problem it is. (If a code is ever added to
// one list and not the other, this stops the build rather than explain the wrong problem)
_Static_assert(PROBLEM_READ_ONLY - PROBLEM_NOT_FOUND == FS_ERR_NOT_FOUND - FS_ERR_READ_ONLY,
               "the PROBLEM_ codes for the file system must match the FS_ERR_ codes, in order");

static problem_t fs_problem (int fs_error) {
    int first = FS_ERR_NOT_FOUND;   // -1: PROBLEM_NOT_FOUND
    int last = FS_ERR_READ_ONLY;    // -9: PROBLEM_READ_ONLY
    if (fs_error > first || fs_error < last) {
        return PROBLEM_OTHER;
    }
    return (problem_t)(PROBLEM_NOT_FOUND + (first - fs_error));
}


// =================================================================================================
//  Printing
// =================================================================================================

// Copy text, writing each "@cli_something" as the name of the command that runs cli_something
// ("@cli_list" -> "LA"). An @ followed by a name no command runs is copied as it is
static void expand_commands (const char* text, char* out, size_t size) {
    size_t used = 0;
    while (*text != '\0' && used + 1 < size) {
        if (*text != '@') {
            out[used++] = *text++;
            continue;
        }
        // the function's name runs from after the @ up to the first character that can't be
        // in a C name
        char function[32];
        size_t length = 0;
        text++;
        while ((isalnum((unsigned char)*text) || *text == '_') && length + 1 < sizeof(function)) {
            function[length++] = *text++;
        }
        function[length] = '\0';

        const char* command = cli_command_called(function);
        const char* word = command != NULL ? command : function;
        size_t room = size - 1 - used;
        size_t word_length = slen(word);
        if (word_length > room) {
            word_length = room;
        }
        memcpy(out + used, word, word_length);
        used += word_length;
    }
    out[used] = '\0';
}

// print text with its words wrapped at WIDTH, every line indented by `indent` spaces
static void print_wrapped (const char* text, int indent) {
    char expanded[640];
    expand_commands(text, expanded, sizeof(expanded));

    prtf("%*s", indent, "");
    int column = indent;
    const char* word = expanded;
    while (*word != '\0') {
        size_t length = strcspn(word, " ");
        bool starts_line = column == indent;
        if (!starts_line && column + 1 + (int)length > WIDTH) {
            prtf("\n%*s", indent, "");
            column = indent;
            starts_line = true;
        }
        if (!starts_line) {
            prtf(" ");
            column++;
        }
        prtf("%.*s", (int)length, word);
        column += (int)length;

        word += length;
        while (*word == ' ') {
            word++;
        }
    }
    prtf("\n");
}

// Print one line that may be too long for the screen, carrying on over as many lines as it
// needs. `column` is where the cursor already is (after a line number, say). Each break is at
// a space if there is one near the end of the room left (otherwise anywhere), and every line
// after the first starts `indent` spaces in. Ends with a newline
static void print_folded (const char* text, size_t length, int column, int indent) {
    for (;;) {
        // (always at least a little room, so even a line that starts at the edge moves on)
        size_t room = column < WIDTH - 10 ? (size_t)(WIDTH - column) : 10;
        if (length <= room) {
            prtf("%.*s\n", (int)length, text);
            return;
        }
        size_t cut = room;
        while (cut > room / 2 && text[cut] != ' ') {
            cut--;
        }
        if (text[cut] != ' ') {
            cut = room; // one long word: just cut it
        }
        prtf("%.*s\n%*s", (int)cut, text, indent, "");
        while (cut < length && text[cut] == ' ') {
            cut++; // the next line doesn't start with the space it was broken at
        }
        text += cut;
        length -= cut;
        column = indent;
    }
}

// Print a comment from the source, indented under a heading. Comments are written with their
// lines broken for a wide editor, so plain lines that follow on from each other are joined
// into paragraphs and wrapped to fit the screen. Indented lines (examples, little tables) and
// blank lines are kept just as they are
static void print_comment (const char* comment) {
    char paragraph[1024] = "";
    for (const char* line = comment; ; ) {
        size_t length = strcspn(line, "\n");
        bool plain = length > 0 && line[0] != ' ';
        bool at_end = *line == '\0';

        // a plain line carries on the paragraph; anything else ends it
        if (plain) {
            if (paragraph[0] != '\0') {
                scat(paragraph, " ", sizeof(paragraph));
            }
            size_t used = slen(paragraph);
            size_t room = sizeof(paragraph) - 1 - used;
            size_t copy = length < room ? length : room;
            memcpy(paragraph + used, line, copy);
            paragraph[used + copy] = '\0';
        } else {
            if (paragraph[0] != '\0') {
                prtf("%*s", INDENT, "");
                print_folded(paragraph, slen(paragraph), INDENT, INDENT);
                paragraph[0] = '\0';
            }
            if (at_end) {
                return;
            }
            // an indented or blank line: as it is (a long one carries on under its own text)
            int own_indent = (int)strspn(line, " ");
            prtf("%*s", INDENT, "");
            print_folded(line, length, INDENT, INDENT + own_indent);
        }

        line += length;
        if (*line == '\n') {
            line++;
        }
    }
}

// a heading, like "What you can do:", in white
static void print_heading (const char* heading) {
    colour(WHITE, console_bg);
    prtf("%s\n", heading);
    colour(BRIGHT, console_bg);
}

// where a function is, for a line like "The code that decides this:"
static void print_place (const char* function, const source_place_t* place) {
    colour(LIGHT_CYAN, console_bg);
    prtf("%s", function);
    colour(BRIGHT, console_bg);
    prtf(" in %s, line %d", place->file->path, place->line);
}


// =================================================================================================
//  2. The last problem
// =================================================================================================

#define CODE_LINES 4     // how many lines of a program WHY shows: two before the error, the
                         // line itself and the one after
#define CODE_WIDTH 72    // longer lines are cut short

static struct {
    problem_t problem;
    char message[160];          // the message as it was printed

    // PROBLEM_NOT_FOUND: the folder and name that were looked for, to find names like it
    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];

    // PROBLEM_PROGRAM: which program, which line, and the lines of code around it
    char program[FS_NAME_MAX];
    int line;
    int first_code_line;        // the line number of code[0]
    int code_lines;             // how many of code[] are used
    char code[CODE_LINES][CODE_WIDTH + 1];
} last;

static int hints_shown = 0;

// keep the folder and name of what a problem was about. "notes.txt" -> the current folder
// and "notes.txt"; "/sys/cach/" -> "/sys/" and "cach"
static void remember_subject (const char* subject) {
    last.folder[0] = '\0';
    last.name[0] = '\0';
    if (subject == NULL) {
        return;
    }
    char path[FS_PATH_MAX];
    scpy(path, subject, sizeof(path));
    size_t length = slen(path);
    while (length > 1 && path[length - 1] == '/') {
        path[--length] = '\0'; // a folder: "/sys/cach/" -> "/sys/cach"
    }
    if (!fs_split_path(path, last.folder, sizeof(last.folder), last.name, sizeof(last.name))) {
        last.folder[0] = '\0';
        last.name[0] = '\0';
    }
}

// print a problem's message and remember it (the shared part of explain_say and explain_say_fs)
static void say (problem_t problem, const char* subject) {
    last.problem = problem;
    last.code_lines = 0;
    remember_subject(subject);

    prtf("%s", last.message);
    if (hints_shown < HINTS) {
        hints_shown++;
        char hint[32];
        sfmt(hint, sizeof(hint), "(%s explains)", cli_command_called("cli_why"));
        // after the message if it fits on the line, otherwise on a line of its own
        bool fits = slen(last.message) + 2 + slen(hint) < COLS;
        colour(DARK_GRAY, console_bg);
        prtf(fits ? "  %s" : "\n  %s", hint);
        colour(BRIGHT, console_bg);
    }
    prtf("\n");
}

void explain_say (problem_t problem, const char* subject, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    vsnprintf(last.message, sizeof(last.message), format, ap);
    va_end(ap);
    say(problem, subject);
}

void explain_say_fs (int fs_error, const char* subject, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    vsnprintf(last.message, sizeof(last.message), format, ap);
    va_end(ap);
    scat(last.message, ": ", sizeof(last.message));
    scat(last.message, fs_strerror(fs_error), sizeof(last.message));
    say(fs_problem(fs_error), subject);
}

void explain_program (const char* program, int line, const char* message, const char* source) {
    last.problem = program != NULL ? PROBLEM_PROGRAM : PROBLEM_TYPED_CODE;
    scpy(last.message, message, sizeof(last.message));
    scpy(last.program, program != NULL ? program : "Your code", sizeof(last.program));
    last.line = line;
    last.code_lines = 0;
    last.folder[0] = '\0';
    if (source == NULL || line < 1) {
        return;
    }

    // keep copies of the lines around the error: the program's source is freed once it stops
    last.first_code_line = line > 2 ? line - 2 : 1;
    int n = 1;
    for (const char* p = source; *p != '\0' && last.code_lines < CODE_LINES; n++) {
        size_t length = strcspn(p, "\n");
        if (n >= last.first_code_line) {
            size_t keep = length;
            if (keep > 0 && p[keep - 1] == '\r') {
                keep--;
            }
            if (keep > CODE_WIDTH) {
                keep = CODE_WIDTH;
            }
            memcpy(last.code[last.code_lines], p, keep);
            last.code[last.code_lines][keep] = '\0';
            last.code_lines++;
        }
        p += length;
        if (*p == '\n') {
            p++;
        }
    }
}

void explain_forget (void) {
    memset(&last, 0, sizeof(last));
    hints_shown = 0;
}

// ---- names like the one that wasn't found ------------------------------------------------------

#define MOST_SIMILAR 5   // how many look-alike names WHY lists
#define SIMILAR 2        // a name this many changes away (or fewer, see sdist) looks alike

typedef struct {
    int found;
} similar_t;

// fs_list() calls this for each thing in the folder: print it if it looks like the name
static void print_if_similar (const char* name, bool is_dir, uint32_t size, void* context) {
    (void)size;
    similar_t* similar = context;
    if (similar->found == MOST_SIMILAR || sdist(name, last.name) > SIMILAR) {
        return;
    }
    prtf("%s%s%s", similar->found == 0 ? "" : ", ", name, is_dir ? "/" : "");
    similar->found++;
}

// for a name that wasn't found: list the names in the same folder that are almost the same
static void print_similar_names (void) {
    if (last.name[0] == '\0' || !fs_dir_exists(last.folder)) {
        return;
    }
    prtf("\n");
    print_heading("Names like it:");
    prtf("%*s", INDENT, "");
    similar_t similar = { 0 };
    fs_list(last.folder, print_if_similar, &similar);
    if (similar.found == 0) {
        prtf("nothing in %s is spelled like %s", last.folder, last.name);
    }
    prtf("\n");
}

// for an AerLang error: the lines of the program around it, with an arrow at the line
static void print_program_lines (void) {
    if (last.code_lines == 0) {
        return;
    }
    prtf("\n");
    for (int i = 0; i < last.code_lines; i++) {
        int line = last.first_code_line + i;
        bool is_it = line == last.line;
        colour(is_it ? LIGHT_RED : DARK_GRAY, console_bg);
        prtf("%s%4d  ", is_it ? "->" : "  ", line);
        colour(is_it ? WHITE : BRIGHT, console_bg);
        prtf("%s\n", last.code[i]);
    }
    colour(BRIGHT, console_bg);
}

void explain_last (void) {
    if (last.problem == PROBLEM_NONE) {
        print_wrapped("Nothing has gone wrong since you logged in. When something does, "
                      "@cli_why on its own explains it.", 0);
        print_wrapped("@cli_why with a name explains that instead: a command (@cli_why DEL), an "
                      "AerLang function (@cli_why GOPEN), or any C function in AerOS "
                      "(@cli_why FS_LIST). @cli_src shows the code.", 0);
        return;
    }
    const explanation_t* explanation = &explanations[last.problem];

    // what was said, then what it means
    colour(YELLOW, console_bg);
    bool aerlang = last.problem == PROBLEM_PROGRAM || last.problem == PROBLEM_TYPED_CODE;
    if (aerlang) {
        prtf("%s stopped", last.program);
        // (a single line typed at the prompt is line 1, which goes without saying)
        bool say_line = last.problem == PROBLEM_PROGRAM ? last.line > 0 : last.line > 1;
        if (say_line) {
            prtf(" on line %d", last.line);
        }
        prtf(": %s\n", last.message);
    } else {
        prtf("%s\n", last.message);
    }
    colour(BRIGHT, console_bg);

    if (explanation->what != NULL) {
        print_wrapped(explanation->what, INDENT);
    }
    print_program_lines();

    prtf("\n");
    print_heading("Why:");
    print_wrapped(explanation->why, INDENT);

    if (last.problem == PROBLEM_NOT_FOUND) {
        print_similar_names();
    }

    prtf("\n");
    print_heading("What you can do:");
    print_wrapped(explanation->fix, INDENT);

    // and where in AerOS this was decided
    const char* code = explanation->code;
    if (fs_mode == FS_MODE_MEMORY && explanation->code_vfs != NULL) {
        code = explanation->code_vfs;
    }
    source_place_t place;
    if (code != NULL && source_find_function(code, &place)) {
        prtf("\n");
        print_heading("The code that decides this:");
        prtf("%*s", INDENT, "");
        print_place(code, &place);
        prtf(".\n%*s%s %s shows it.\n", INDENT, "", cli_command_called("cli_src"), code);
    }
}


// =================================================================================================
//  3. Reading the source: WHY <function> and SRC <function>
// =================================================================================================

// where to find what a function is for: the comment above its definition, or if that has
// none, the comment above its declaration in a .h file. "" if neither has one
static void function_comment (const char* function, const source_place_t* place, char* out, size_t size) {
    source_comment_above(place->file, place->line, out, size);
    source_place_t declared;
    if (out[0] == '\0' && source_find_declaration(function, &declared)) {
        source_comment_above(declared.file, declared.line, out, size);
    }
}

// The first sentence of a function's comment, on one line and cut to `width` characters:
// for the list of what a function uses
static void function_summary (const char* function, const source_place_t* place, char* out, size_t width) {
    char comment[512];
    function_comment(function, place, comment, sizeof(comment));

    // one line: newlines become spaces
    for (char* c = comment; *c != '\0'; c++) {
        if (*c == '\n') {
            *c = ' ';
        }
    }
    // the first sentence: up to ". " (or the end)
    char* full_stop = strstr(comment, ". ");
    if (full_stop != NULL) {
        full_stop[1] = '\0';
    }
    // cut short, with "..." to show it was
    if (slen(comment) > width) {
        comment[width - 3] = '\0';
        scat(comment, "...", sizeof(comment));
    }
    scpy(out, comment, width + 1);
}

// ---- what a function uses ------------------------------------------------------------------------

#define MOST_USES 14     // the longest list WHY shows
#define USE_NAME_MAX 40

typedef struct {
    char names[MOST_USES][USE_NAME_MAX];
    source_place_t places[MOST_USES];
    int count;
} uses_t;

// words that are followed by '(' in C but aren't functions
static bool is_keyword (const char* name) {
    static const char* const keywords[] = { "if", "for", "while", "switch", "return", "sizeof", "defined" };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (seq(name, keywords[i])) {
            return true;
        }
    }
    return false;
}

// add a name to the list if it is an AerOS function worth listing. Left out: the function
// itself, names already listed, and the everyday C library ones (lib/aer_string.c,
// aer_stdio.c...: prtf, scpy, malloc), which almost everything uses
static void add_use (uses_t* uses, const char* name, const char* self) {
    if (uses->count == MOST_USES || is_keyword(name) || seq(name, self)) {
        return;
    }
    for (int i = 0; i < uses->count; i++) {
        if (seq(uses->names[i], name)) {
            return;
        }
    }
    source_place_t place;
    if (!source_find_function(name, &place)) {
        return; // not a function (a macro, or a function pointer)
    }
    if (strncmp(place.file->path, "lib/aer_", 8) == 0) {
        return;
    }
    scpy(uses->names[uses->count], name, USE_NAME_MAX);
    uses->places[uses->count] = place;
    uses->count++;
}

// Find the functions a piece of code calls: every name that has a '(' after it, outside
// comments, strings and characters. Not "x.run(...)" or "x->run(...)" though: those call
// whatever function a variable points to, which can't be looked up by name
static void gather_uses (const char* code, const char* end, const char* self, uses_t* uses) {
    const char* p = code;
    while (p < end) {
        // skip what can't hold a call
        if (p + 1 < end && p[0] == '/' && p[1] == '/') {
            while (p < end && *p != '\n') {
                p++;
            }
            continue;
        }
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) {
                p++;
            }
            p += 2;
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            while (p < end && *p != quote) {
                p += *p == '\\' ? 2 : 1; // a backslash escapes the next character
            }
            p++;
            continue;
        }
        bool starts_name = isalpha((unsigned char)*p) || *p == '_';
        if (!starts_name) {
            // (a number like 0x1F is skipped whole, so its "x1F" isn't taken for a name)
            if (isdigit((unsigned char)*p)) {
                while (p < end && (isalnum((unsigned char)*p) || *p == '_')) {
                    p++;
                }
            } else {
                p++;
            }
            continue;
        }

        // a name: is it called?
        const char* name = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '_')) {
            p++;
        }
        size_t length = (size_t)(p - name);
        const char* after = p;
        while (after < end && *after == ' ') {
            after++;
        }
        bool member = (name > code && name[-1] == '.') || (name - 1 > code && name[-2] == '-' && name[-1] == '>');
        bool called = after < end && *after == '(';
        if (called && !member && length < USE_NAME_MAX) {
            char copy[USE_NAME_MAX];
            memcpy(copy, name, length);
            copy[length] = '\0';
            add_use(uses, copy, self);
        }
    }
}

// the text of lines `first` to `last` of a file, as a start and an end pointer
static bool lines_text (const source_file_t* file, int first, int last, const char** start, const char** end) {
    size_t length;
    *start = source_line(file, first, &length);
    const char* last_line = source_line(file, last, &length);
    if (*start == NULL || last_line == NULL) {
        return false;
    }
    *end = last_line + length;
    return true;
}

bool explain_function (const char* function) {
    source_place_t place;
    if (!source_find_function(function, &place)) {
        return false;
    }

    // where it is, and what the comment above it says
    print_place(function, &place);
    prtf(":\n");
    char comment[2048];
    function_comment(function, &place, comment, sizeof(comment));
    if (comment[0] == '\0') {
        prtf("%*s(there is no comment about it)\n", INDENT, "");
    }
    print_comment(comment);

    // what it uses, each with the first sentence of its own comment
    int last_line = source_function_end(place.file, place.line);
    const char* body;
    const char* body_end;
    uses_t uses = { .count = 0 };
    if (lines_text(place.file, place.line + 1, last_line, &body, &body_end)) {
        gather_uses(body, body_end, function, &uses);
    }
    if (uses.count > 0) {
        prtf("\n");
        print_heading("It uses:");
        int name_width = 0;
        for (int i = 0; i < uses.count; i++) {
            int length = (int)slen(uses.names[i]);
            name_width = length > name_width ? length : name_width;
        }
        for (int i = 0; i < uses.count; i++) {
            char summary[WIDTH + 1];
            int room = WIDTH - INDENT - name_width - 2;
            function_summary(uses.names[i], &uses.places[i], summary, room > 10 ? (size_t)room : 10);
            colour(LIGHT_CYAN, console_bg);
            prtf("%*s%-*s", INDENT, "", name_width, uses.names[i]);
            colour(BRIGHT, console_bg);
            prtf("  %s\n", summary);
        }
    }

    colour(DARK_GRAY, console_bg);
    prtf("\n%s <name> explains any of those, %s %s shows the code.\n",
         cli_command_called("cli_why"), cli_command_called("cli_src"), function);
    colour(BRIGHT, console_bg);
    return true;
}

bool explain_show_code (const char* function) {
    source_place_t place;
    if (!source_find_function(function, &place)) {
        return false;
    }
    int first = source_comment_start(place.file, place.line);
    int last_line = source_function_end(place.file, place.line);

    colour(YELLOW, console_bg);
    prtf("%s, lines %d to %d\n", place.file->path, first, last_line);
    for (int n = first; n <= last_line; n++) {
        size_t length;
        const char* text = source_line(place.file, n, &length);
        if (text == NULL) {
            break;
        }
        // comment lines in green, like most code editors show them
        size_t indent = 0;
        while (indent < length && text[indent] == ' ') {
            indent++;
        }
        bool is_comment = indent + 1 < length && text[indent] == '/' && (text[indent + 1] == '/' || text[indent + 1] == '*');
        bool inside_block = n < place.line && n > first && !is_comment; // the lines of a /* */ block
        colour(DARK_GRAY, console_bg);
        prtf("%4d  ", n);
        colour(is_comment || inside_block ? GREEN : BRIGHT, console_bg);
        // a line too long for the screen carries on under the code, clear of the line numbers
        int number_width = 6;
        print_folded(text, length, number_width, number_width + (int)indent + 4);
    }
    colour(DARK_GRAY, console_bg);
    if (last_line - first > ROWS - 4) {
        prtf("(Page Up scrolls back to the start)\n");
    }
    colour(BRIGHT, console_bg);
    return true;
}

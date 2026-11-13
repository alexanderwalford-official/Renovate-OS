/*
    cmd_help.c - the terminal's commands that explain AerOS

        DOC                  read the manual (the files in /docs/) in the reader app
        WHY                  explain the last problem, or a command or function, from its comments
        SRC                  show the code behind a command or function, or a whole source file
        SHOW                 run a command, then show step by step what AerOS did inside

    WHY and SRC read AerOS's own source code, built in as the read-only folder /src/
    (sys/source.h), and sys/explain.c does the explaining. HLP, the list of commands, is in
    t_int.c beside the table it prints. commands.h says how the files of the terminal fit
    together.
*/

#include "commands.h"
#include <stddef.h>
#include "../../lib/primary_definitions.h"
#include "../../drivers/video/VGA_linear.h"
#include "../f_sys.h"
#include "../explain.h"
#include "../source.h"
#include "../trace.h"
#include "t_int.h"
#include "../../lang/aerlang.h"


// =================================================================================================
//  The manual
// =================================================================================================

// DOC [TOPIC]: open the manual in the reader app. "doc sound" opens /docs/sound.md
void cli_docs (char* arguments) {
    char path[FS_PATH_MAX];

    if (arguments[0] == '\0') {
        scpy(path, "/docs/index.md", sizeof(path));
    } else if (strchr(arguments, '.') != NULL) {
        sfmt(path, sizeof(path), "/docs/%s", arguments);     // "docs sample.htm"
    } else {
        sfmt(path, sizeof(path), "/docs/%s.md", arguments);  // "docs sound"
    }
    aer_run_file("/bin/read.aer", path);
}


// =================================================================================================
//  WHY and SRC: AerOS explains itself (the explaining is done in sys/explain.c)
// =================================================================================================

// The C function behind a name someone typed after WHY or SRC:
//     a command           "del"     ->  "cli_delete"    (*command is set to it)
//     an AerLang function "gopen"   ->  "bi_gfx_open"
//     anything else       "fs_list" ->  "fs_list", taken to be a C function's name
// The function's name is written into `out`
static void code_behind (const char* name, char* out, size_t size, const command_t** command) {
    *command = find_command(name);
    const char* builtin = aer_builtin_function(name);
    if (*command != NULL) {
        scpy(out, (*command)->function, size);
    } else if (builtin != NULL) {
        scpy(out, builtin, size);
    } else {
        scpy(out, name, size);
    }
}

// C is case sensitive, but commands aren't: so if "FS_LIST" isn't a C function, try "fs_list"
static void lower_case_if_unknown (char* function) {
    source_place_t place;
    if (!source_find_function(function, &place)) {
        strlwr(function);
    }
}

// WHY [NAME]: on its own, explain the last thing that went wrong. With a name, explain that
// instead: a command (WHY DEL), an AerLang function (WHY GOPEN) or a C function (WHY FS_LIST),
// from the comments written above them in AerOS's own source code
void cli_why (char* arguments) {
    if (arguments[0] == '\0') {
        explain_last();
        return;
    }
    // turn what was typed into a C function's name ("del" -> "cli_delete"), then make sure
    // that function really is in the source before saying anything about it
    char function[64];
    const command_t* command;
    code_behind(arguments, function, sizeof(function), &command);
    lower_case_if_unknown(function);

    source_place_t place;
    if (!source_find_function(function, &place)) {
        prtf("AerOS has no command, AerLang function or C function called %s.\n", arguments);
        return;
    }

    // say what the name is first, then let explain_function() take over. Each line here
    // stops part way, "The command runs " or "It is ", for explain_function to finish with
    // the function and where it is. A plain C function needs no introduction
    if (command != NULL) {
        char usage[64];
        command_usage(command, usage, sizeof(usage));
        colour(YELLOW, console_bg);
        prtf("%s - %s\n", usage, command->description);
        colour(BRIGHT, console_bg);
        prtf("The command runs ");
    } else if (aer_builtin_function(arguments) != NULL) {
        colour(YELLOW, console_bg);
        prtf("%s() is a built-in AerLang function.\n", arguments);
        colour(BRIGHT, console_bg);
        prtf("It is ");
    }
    explain_function(function);
}

// SRC <NAME>: show the code behind a command (SRC DEL), an AerLang function (SRC GOPEN) or a
// C function (SRC FS_LIST), with its line numbers. Or open a whole source file in the reader
// (SRC FAT.C, SRC SYS/FAT.C)
void cli_src (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage("e.g. SRC DEL, SRC FS_LIST or SRC FAT.C");
        return;
    }
    // first as a function: explain_show_code prints it and returns true if there is one
    char function[64];
    const command_t* command;
    code_behind(arguments, function, sizeof(function), &command);
    lower_case_if_unknown(function);
    if (explain_show_code(function)) {
        return;
    }

    // not a function: a file? The source files' paths start below /src/ ("sys/fat.c"), so the
    // folder goes on the front before the reader is given it
    const source_file_t* file = source_search(arguments);
    if (file != NULL) {
        char path[FS_PATH_MAX];
        sfmt(path, sizeof(path), "%s%s", SOURCE_FOLDER, file->path);
        aer_run_file("/bin/read.aer", path);
        return;
    }
    prtf("AerOS has no command, function or source file called %s.\n", arguments);
    prtf("(%s /src/ then %s lists the source files.)\n", command_name(cli_set_directory), command_name(cli_list));
}


// =================================================================================================
//  Watching AerOS work
// =================================================================================================

// SHOW <COMMAND>: run a command (or an app, or a line of AerLang) as if it had been typed, then
// show step by step what AerOS did inside to carry it out: which part decided what, and why.
// Each step names the C function that took the note, so WHY and SRC can tell you more about it.
// The notes are TRACE() lines in the code (see sys/trace.h)
void cli_show (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage("e.g. SHOW DEL notes.txt, SHOW RD /docs/index.md or SHOW hello");
        return;
    }
    // HandleInput cuts the line up as it goes, and `arguments` is part of this command's line
    char line[CLI_INPUT_MAX];
    scpy(line, arguments, sizeof(line));

    // everything from here to trace_end() is noted down, then listed
    trace_begin();
    HandleInput(line);
    trace_end();
    prtf("\n");
    trace_print();
}

/*
    commands.h - what the files of the terminal share with each other (only for sys/terminal/)

    The terminal is split into a few files, so that each one is a comfortable read:

        t_int.c          the terminal itself: starting it, the prompt, reading a line and
                         running it, Tab completion, "Did you mean...?", AerLang typed at the
                         prompt, and the table of every command (with HLP, which prints it)
        cmd_files.c      LA SD RD MKF WR MKD DEL VERS UNDO ENC DEC EDT
        cmd_system.c     ECH CLS CLK UP MEM DRV BP PLY RES RT RBT OFF
        cmd_network.c    NET FW MSG LSN
        cmd_programs.c   AER RUN PM QUE GO, and running an app by its name ("hello")
        cmd_database.c   SQL (AerDB databases, db/aerdb.h)
        cmd_help.c       DOC WHY SRC SHOW
        cmd_accounts.c   ME USRS MKU DELU PW OUT PERM
        editor.c         the text editor that EDT opens

    t_int.h is the terminal's face to the rest of AerOS. This file is only for the files above:
    the shape of a command, the few helpers every command file uses, and the function behind
    every command, so the table in t_int.c can point to them all.

    ADDING A COMMAND

        1. Write its function in the cmd_ file it belongs with, with a comment directly above
           it saying what it does (WHY shows that comment):

               // HI <NAME>: say hello
               void cli_hello (char* arguments) {
                   prtf("Hello, %s!\n", arguments);
               }

        2. Declare it below, with the others from that file.
        3. Add one COMMAND row for it to the table in t_int.c: its name, what goes after it,
           and the line HLP shows for it. HLP, Tab completion and WHY all find it from there.
*/

#ifndef COMMANDS_H
#define COMMANDS_H

#include <stdbool.h>
#include <stddef.h>

// the longest password the terminal asks for, counting the '\0'
#define CLI_PASSWORD_MAX 64

// the longest line the user can type, counting the '\0' that ends it
#define CLI_INPUT_MAX 128

// One command, a row of the table in t_int.c
typedef struct {
    const char* name;          // what the user types, in lower case: "del" (DEL and Del work too)
    const char* arguments;     // what goes after it, for HELP and "Usage:": "<NAME>", or "" for nothing
    const char* description;   // the line HELP shows for it
    void (*run)(char* arguments);
    const char* function;      // run's name, "cli_delete", for WHY and SRC to find its code
} command_t;

// true while someone is logged in. Defined in t_int.c, whose loop goes back to the login screen
// when OUT (cmd_accounts.c) sets it to false
extern bool cli_logged_in;


// ---- helpers in t_int.c, for every command file -------------------------------------------------

// write a command's name in capitals followed by its arguments, e.g. "DEL <NAME>", into out
void command_usage (const command_t* command, char* out, size_t size);

// print the running command's usage line, like "Usage: DEL <NAME>", plus an optional hint
// after it (hint may be NULL)
void show_usage (const char* hint);

// a command's name in capitals, found by the function it runs: command_name(cli_decrypt) gives
// "DEC". Messages that mention another command use it, so they follow a rename in the table
const char* command_name (void (*run)(char* arguments));

// find a command in the table by name, ignoring upper / lower case. NULL if there is none
const command_t* find_command (const char* name);

// split "notes.txt some text here" into the first word and the rest, cutting the line in place.
// *rest is "" if there was nothing after the first word
char* split_first_word (char* line, char** rest);

// ask a question and read the answer without showing it (each key shows as *)
void ask_secret (const char* question, char* answer, int size);


// ---- in cmd_programs.c, for t_int.c -------------------------------------------------------------

// a word that isn't a command may be an app: "hello" runs /bin/hello.aer.
// Returns false if there is no program by that name
bool run_program (const char* program, const char* arguments);


// ---- the commands, file by file. Each one is given everything typed after its name --------------

// t_int.c
void cli_help (char* arguments);

// cmd_files.c
void cli_encrypt (char* arguments);
void cli_decrypt (char* arguments);
void cli_list (char* arguments);
void cli_set_directory (char* arguments);
void cli_read_file (char* arguments);
void cli_make_file (char* arguments);
void cli_write_file (char* arguments);
void cli_make_directory (char* arguments);
void cli_delete (char* arguments);
void cli_versions (char* arguments);
void cli_undo (char* arguments);
void cli_edit (char* arguments);

// cmd_system.c
void cli_echo (char* arguments);
void cli_clear (char* arguments);
void cli_clock (char* arguments);
void cli_uptime (char* arguments);
void cli_memory (char* arguments);
void cli_drives (char* arguments);
void cli_beep (char* arguments);
void cli_reboot (char* arguments);
void cli_shutdown (char* arguments);
void cli_resolution (char* arguments);
void cli_play (char* arguments);
void cli_realtime (char* arguments);

// cmd_network.c
void cli_network (char* arguments);
void cli_firewall (char* arguments);
void cli_message (char* arguments);
void cli_listen (char* arguments);

// cmd_programs.c
void cli_aer (char* arguments);
void cli_run (char* arguments);
void cli_pman (char* arguments);
void cli_queue (char* arguments);
void cli_go (char* arguments);

// cmd_database.c
void cli_sql (char* arguments);

// cmd_help.c
void cli_docs (char* arguments);
void cli_why (char* arguments);
void cli_src (char* arguments);
void cli_show (char* arguments);

// cmd_accounts.c
void cli_whoami (char* arguments);
void cli_users (char* arguments);
void cli_add_user (char* arguments);
void cli_delete_user (char* arguments);
void cli_password (char* arguments);
void cli_logout (char* arguments);
void cli_permissions (char* arguments);

#endif /* COMMANDS_H */

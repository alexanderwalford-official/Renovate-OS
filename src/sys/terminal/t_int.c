/*
    t_int.c - the AerOS terminal (the command line)

    This is where the user spends their time once AerOS has started. kernel.c creates a process
    called "terminal", the process manager runs it, and that calls cli_init(). From then on the
    loop in cli_init() *is* the operating system:

        forever:
            show the login screen                 auth_login(), sys/accounts/auth.c
            draw the banner                       cli_rend_main()
            until the user logs out:
                print the prompt                  cli_prompt(), e.g. "alex@0//home/alex/: "
                read a line                       GetLine(), drivers/input/IO.c
                run it                            HandleInput()

    HOW A COMMAND RUNS

    HandleInput() splits the line into the first word (the command) and the rest (its
    arguments), then looks the command up in the table called `commands` near the bottom of
    this file. Each entry there says:

        the name you type        "del"
        what goes after it       "<NAME>"
        a one line description   "DELETE A FILE, OR A DIRECTORY AND ALL IN IT (ASKS FIRST)"
        the function to call     cli_delete

    If the word isn't in the table, it is tried as the name of an AerLang app ("hello" runs
    /bin/hello.aer), and then the whole line as AerLang code ("let x = 2", "x * 10"), which
    runs in the shell's session (lang/aer_interp.c, "The shell"). A word that is none of those
    gets "Did you mean ...?" with the closest command or app.

    WHEN SOMETHING GOES WRONG

    Commands report problems with explain_say() (sys/explain.h) rather than a plain prtf, so
    AerOS remembers the problem and WHY can explain it afterwards. WHY DEL and SRC DEL explain
    and show the code of a command, read from AerOS's own source in /src/.

    ADDING OR RENAMING A COMMAND

    Everything about a command's name comes from that one table: HELP, Tab completion, the
    "Usage:" messages and WHY's explanations all read it. So to rename a command, change its
    name in the table and nothing else. To add one, write a function like this in the cmd_
    file it belongs with, declare it in commands.h, and add a line to the table:

        void cli_hello (char* arguments) {
            prtf("Hello, %s!\n", arguments);
        }

    `arguments` is everything after the command, with the spaces at both ends removed. It is
    an empty string ("") when nothing was typed after the command, never NULL.

    FULL SCREEN PROGRAMS

    Some programs take over the whole screen (the editor, the picture viewer, the red error
    screen). They all start with clear(), which counts up vga_clear_count. HandleInput() checks
    that count before and after each command, and redraws the banner if it changed, so the
    terminal always comes back looking right.

    WHERE THINGS ARE

    This file is the terminal itself: starting it, the prompt, the command table, Tab
    completion and running a line. The commands' own functions are in the cmd_ files beside
    it, one file to a topic (files, the system, the network, programs, help, accounts).
    commands.h says which file holds which command, and is what joins them to the table here.
*/

#include "t_int.h"
#include "commands.h"
#include <stdint.h>
#include <stddef.h>
#include "../../lib/primary_definitions.h"
#include "../../drivers/video/VGA_linear.h"
#include "../../drivers/input/IO.h"
#include "../sys_clck.h"
#include "../fat.h"
#include "../f_sys.h"
#include "../boot.h"
#include "../../lang/aerlang.h"
#include "../accounts/auth.h"
#include "../explain.h"


// true once the terminal has started. compile() (lib/aer_comp.c) checks it so it never starts
// a second terminal while this one is running
bool cli_active = false;

// set to false by the logout command (OUT, in cmd_accounts.c), which sends the terminal back to
// the login screen. Shared with it through commands.h
bool cli_logged_in = false;

// the line the user is typing, re-used for every command
static char cli_input[CLI_INPUT_MAX];


// =================================================================================================
//  The command table, part 1: its shape
// =================================================================================================
//
// The table itself is near the bottom of the file, after the functions it uses. Its shape,
// command_t, is in commands.h, because the command files need it too (WHY and SRC look
// commands up in the table).

// One row of the table. #run turns the function into its name as a string ("cli_delete"):
// C can't find a function's name from a pointer to it, so the table keeps both
#define COMMAND(name, arguments, description, run) { name, arguments, description, run, #run }

// the command that is running right now, so show_usage() knows which one to explain
static const command_t* running_command = NULL;

// what Tab does, further down. (C reads a file from top to bottom, so a function used before
// it is written out in full has to be declared first)
static int cli_complete (char* line, int length, int size);


// =================================================================================================
//  Small helpers used by many commands
// =================================================================================================
//
// Shared with the cmd_ files, which find them through commands.h

// write a command's name in capitals followed by its arguments, e.g. "DEL <NAME>", into out
void command_usage (const command_t* command, char* out, size_t size) {
    // the name, one letter at a time in capitals, stopping one short of the end of `out` so
    // there is always room for the '\0'
    size_t i = 0;
    while (command->name[i] != '\0' && i < size - 1) {
        out[i] = (char)toupper((unsigned char)command->name[i]);
        i++;
    }
    out[i] = '\0';

    // then its arguments after a space, unless it takes none (so HELP shows "LA", not "LA ")
    if (command->arguments[0] != '\0') {
        scat(out, " ", size);
        scat(out, command->arguments, size);
    }
}

// print the running command's usage line, like "Usage: DEL <NAME>", plus an optional hint
// after it (hint may be NULL). Every command uses this when it was typed without what it needs
void show_usage (const char* hint) {
    // only set while HandleInput() is running a command, so there is nothing to say outside one
    if (running_command == NULL) {
        return;
    }
    char usage[64];
    command_usage(running_command, usage, sizeof(usage));

    if (hint != NULL) {
        prtf("Usage: %s   (%s)\n", usage, hint);
    } else {
        prtf("Usage: %s\n", usage);
    }
}

// ask a question and read the answer without showing it (each key shows as *)
void ask_secret (const char* question, char* answer, int size) {
    prtf("%s", question);
    GetSecret(answer, size);
}

// split "notes.txt some text here" into the first word ("notes.txt") and the rest
// ("some text here", with its spaces kept). The line is cut in place. *rest is "" if there was
// nothing after the first word
char* split_first_word (char* line, char** rest) {
    // static, so the "" that *rest points at is still there after this function returns
    // (a plain local would be gone). Commands may write into *rest, so it isn't a literal
    static char nothing[1] = "";
    // split into at most 2 pieces: the first space cuts the line, any later ones stay in rest
    char* words[2] = { line, NULL };
    int word_count = split(line, ' ', words, 2);

    if (word_count > 1) {
        *rest = trim(words[1]);
    } else {
        *rest = nothing;
    }
    return words[0];
}


// =================================================================================================
//  Starting the terminal, the banner and the prompt
// =================================================================================================

// flash a solid block at the cursor: `times` flashes, half a second on and half a second off.
// `already_done` says how many have happened already (the loop carries on from there).
// The OS waits while it flashes. Returns 0
int flash_pointer (int times, int already_done) {
    uint8_t x, y;
    console_get_cursor(&x, &y);

    for (int flash = already_done; flash < times; flash++) {
        // a space drawn with the colours swapped is a solid block in the text colour...
        putc(x, y, console_bg, console_fg, ' ');
        clck_hang(500);
        // ...and drawn normally it is a blank cell again
        putc(x, y, console_fg, console_bg, ' ');
        clck_hang(500);
    }
    return 0;
}

// start the terminal. ask_login = 1 shows the login screen first (it only appears when there
// is a disk to keep accounts on), 0 skips it and runs as "guest".
// This loop only ends if cli_active goes false, which nothing does yet: RBT and OFF
// never come back, and logging out only ends the inner loop, back to the login screen
int cli_init (int ask_login) {
    cli_active = true;

    while (cli_active) {
        if (ask_login) {
            auth_login(); // comes back once someone has logged in
        }
        cli_logged_in = true;
        aer_shell_start(); // the prompt's own AerLang variables, kept until logging out
        cli_rend_main();

        while (cli_active && cli_logged_in) {
            cli_prompt();
            // wait for Enter. Tab fills in names (cli_complete), the up and down arrows bring
            // back earlier lines, Page Up and Page Down scroll back through the output.
            // With no keyboard at all this returns an empty line at once, so the prompt repeats
            GetLine(cli_input, sizeof(cli_input), cli_complete);
            HandleInput(cli_input);
        }
    }
    return 0;
}

// clear the screen and draw the terminal's banner. A coloured line says where files are kept:
//   green    DISK mode: an AEROS disk was found and files are saved on it
//   yellow   DEMO mode: picked in the boot options, the disks are never touched
//   red      VFS mode: no usable disk, files only last until the computer is turned off
// Also used to bring the terminal back after a full screen program (see HandleInput). Returns 0
int cli_rend_main (void) {
    // console_clear() fills the screen with the current colours, so set those first
    colour(WHITE, BLACK);
    console_clear();
    // the title in big letters, three quarters of big_text's size on a framebuffer. It is
    // drawn in place, so the cursor is then moved below it, or the text would print over it
    uint8_t title_rows;
    big_text_sized(2, 1, ORANGE, BLACK, "aerOS", 75, &title_rows);
    console_set_cursor(0, (uint8_t)(1 + title_rows + 1));
    colour(ORANGE, BLACK);
    prtf("Version " AEROS_VERSION "\n");
    colour(BRIGHT, BLACK);
    prtf("Alexander Walford 2026\n\n");

    if (fs_mode == FS_MODE_DISK) {
        colour(LIGHT_GREEN, BLACK);
        prtf("[ * ] You are in DISK mode, your files are saved to the %s disk.\n", fat_vol.label);
    } else if (boot_option == BOOT_OPT_DEMO) {
        colour(YELLOW, BLACK);
        prtf("[ ! ] You are in DEMO mode, no disks are loaded and files will not be written.\n");
    } else {
        colour(RED, BLACK);
        prtf("[ ! ] You are in VFS mode, your files will not be written.\n");
    }

    // if someone typed wrong passwords for this account since its last login, say so (once)
    if (auth_failures_seen > 0) {
        colour(YELLOW, BLACK);
        prtf("[ ! ] %u wrong password(s) were typed for %s since your last login.\n",
               auth_failures_seen, auth_user);

        uint32_t times_wiped = auth_failures_seen / AUTH_MAX_FAILURES;
        if (times_wiped > 0) {
            prtf("[ ! ] Your home folder was wiped %u time(s) because of them.\n", times_wiped);
        }
        auth_failures_seen = 0;
    }

    // the normal terminal colours, light grey on black, for everything after the banner
    colour(BRIGHT, BLACK);
    prtf("Type %s for a list of commands, or %s to read the manual.\n\n",
         command_name(cli_help), command_name(cli_docs));

    // forget keys pressed while something else had the screen (the splash screen, a menu), so
    // they don't turn up at the start of the next command, and make sure keys are kept again
    ClearInputbuffer();
    InputBufferChangeState(1);
    return 0;
}

// the prompt: who is logged in, "@0/", then the current folder, which always starts and ends
// with '/'. e.g. "alex@0//: " at the top folder, "alex@0//home/alex/: " in a home folder
void cli_prompt (void) {
    colour(LIGHT_GREEN, BLACK);
    prtf("%s", auth_user);
    colour(BRIGHT, BLACK);
    prtf("@0/%s: ", cd);
}


// =================================================================================================
//  The command table, part 2: the commands
// =================================================================================================
//
// In the order HELP lists them. Names are matched without caring about upper or lower case.
// Columns: name, what goes after it, what it does, the function that runs it.

// The names are short on purpose, and built so they can be guessed:
//     rd read, wr write, mk make, del delete, f file, d folder (directory), u user,
//     and the rest are the word with letters left out: ply (play), rbt (reboot), edt (edit)...
// (AerLang's built-in functions use the same pieces: rdf, wrf, isf...)

static const command_t commands[] = {
    COMMAND("hlp",  "",           "THIS COMMAND (HELP)",                                         cli_help),
    COMMAND("la",   "",           "LIST ALL FILES AND DIRECTORIES IN THE CURRENT DIRECTORY",     cli_list),
    COMMAND("sd",   "<DIR>",      "SET THE CURRENT DIRECTORY (e.g. SD /sys, SD .., SD /)",       cli_set_directory),
    COMMAND("clk",  "",           "PRINTS THE SYSTEM TIME (CLOCK)",                              cli_clock),
    COMMAND("up",   "",           "PRINTS THE TIME SINCE BOOT (UPTIME)",                         cli_uptime),
    COMMAND("pm",   "",           "PRINTS THE PROCESS STACK (PROCESS MANAGER)",                  cli_pman),
    COMMAND("que",  "[PROGRAM]",  "QUEUE A PROGRAM FOR GO, OR LIST THE QUEUE (QUE CLEAR)",       cli_queue),
    COMMAND("go",   "",           "RUN EVERYTHING IN THE QUEUE, IN ORDER (CTRL+C STOPS IT)",     cli_go),
    COMMAND("run",  "<CODE>",     "RUN CODE AS A PROCESS (e.g. run print_stack, run s:hi)",      cli_run),
    COMMAND("rd",   "<FILE>",     "READ A FILE",                                                 cli_read_file),
    COMMAND("sql",  "<FILE> [SQL]", "RUN SQL ON A DATABASE (SQL SHOP.DB SELECT * FROM ITEMS)",     cli_sql),
    COMMAND("mkf",  "<FILE> [T]", "MAKE A FILE, WITH OPTIONAL TEXT",                             cli_make_file),
    COMMAND("wr",   "<FILE> <T>", "WRITE A LINE OF TEXT ON THE END OF A FILE",                   cli_write_file),
    COMMAND("mkd",  "<DIR>",      "MAKE A DIRECTORY",                                            cli_make_directory),
    COMMAND("del",  "<NAME>",     "DELETE A FILE, OR A DIRECTORY AND ALL IN IT (ASKS FIRST)",    cli_delete),
    COMMAND("vers", "[FILE]",     "LIST A FILE'S OLD VERSIONS (RD NOTES.TXT@1 SHOWS ONE)",       cli_versions),
    COMMAND("undo", "<FILE> [N]", "BRING A FILE BACK AS IT WAS BEFORE ITS LAST CHANGE",          cli_undo),
    COMMAND("ech",  "<TEXT>",     "PRINT TEXT (ECHO)",                                           cli_echo),
    COMMAND("cls",  "",           "CLEAR THE SCREEN",                                            cli_clear),
    COMMAND("mem",  "",           "MEMORY USAGE",                                                cli_memory),
    COMMAND("drv",  "",           "LIST IDE / ATA DRIVES",                                       cli_drives),
    COMMAND("bp",   "[HZ]",       "BEEP THE PC SPEAKER",                                         cli_beep),
    COMMAND("ply",  "<FILE>",     "PLAY A .WAV, .MP3 OR .MPG VIDEO (ESC TO STOP)",               cli_play),
    COMMAND("net",  "[IP ...]",   "THE NETWORK: CARD, ADDRESS, WHAT WAS SENT AND KEPT OUT",      cli_network),
    COMMAND("fw",   "[ALLOW ...]","THE FIREWALL: FW ALLOW <ADDRESS OR NAME> <PORT>, FW DEL <N>", cli_firewall),
    COMMAND("msg",  "<TO> <PORT>","SEND A SEALED MESSAGE (e.g. MSG 10.0.2.2 5000 HELLO)",        cli_message),
    COMMAND("lsn",  "<PORT>",     "SHOW THE SEALED MESSAGES THAT ARRIVE ON A PORT (ESC STOPS)",  cli_listen),
    COMMAND("rt",   "[STRESS]",   "RT MODE'S REAL-TIME TASKS (EXPERIMENTAL, FROM THE BOOT MENU)", cli_realtime),
    COMMAND("rbt",  "",           "RESTART THE COMPUTER (REBOOT)",                               cli_reboot),
    COMMAND("off",  "",           "TURN OFF THE COMPUTER (SHUT DOWN)",                           cli_shutdown),
    COMMAND("res",  "[WxH]",      "SHOW OR CHANGE THE SCREEN RESOLUTION (e.g. RES 1280x720)",    cli_resolution),
    COMMAND("edt",  "<FILE>",     "EDIT A FILE (CTRL+S SAVE, CTRL+R RUN A .AER, CTRL+Q QUIT)",   cli_edit),
    COMMAND("aer",  "",           "THE AERLANG PROMPT, TYPE CODE TO RUN IT",                     cli_aer),
    COMMAND("doc",  "[TOPIC]",    "READ THE MANUAL (e.g. DOC SOUND), READ <FILE> FOR ANY FILE",  cli_docs),
    COMMAND("why",  "[NAME]",     "EXPLAIN THE LAST PROBLEM, OR A COMMAND OR FUNCTION (WHY DEL)", cli_why),
    COMMAND("src",  "<NAME>",     "SHOW THE CODE BEHIND A COMMAND OR FUNCTION (e.g. SRC DEL)",   cli_src),
    COMMAND("show", "<COMMAND>",  "RUN A COMMAND, THEN SHOW WHAT AEROS DID INSIDE (SHOW DEL X)", cli_show),
    COMMAND("perm", "[DEL <N>]",  "LIST OR FORGET WHAT PROGRAMS MAY ALWAYS DO TO YOUR FILES",    cli_permissions),
    COMMAND("enc",  "<FILE>",     "ENCRYPT A FILE WITH A PASSWORD (RD ASKS FOR IT TO SHOW IT)",  cli_encrypt),
    COMMAND("dec",  "<FILE>",     "DECRYPT AN ENCRYPTED FILE",                                   cli_decrypt),
    COMMAND("me",   "",           "PRINT WHO IS LOGGED IN",                                      cli_whoami),
    COMMAND("usrs", "",           "LIST THE USER ACCOUNTS",                                      cli_users),
    COMMAND("mku",  "<NAME>",     "MAKE A NEW USER ACCOUNT",                                     cli_add_user),
    COMMAND("delu", "<NAME>",     "DELETE A USER ACCOUNT",                                       cli_delete_user),
    COMMAND("pw",   "",           "CHANGE YOUR PASSWORD",                                        cli_password),
    COMMAND("out",  "",           "LOG OUT, BACK TO THE LOGIN SCREEN",                           cli_logout),
};

// how many commands there are: the size of the whole table divided by the size of one entry
static const size_t command_count = sizeof(commands) / sizeof(commands[0]);

// find the command that runs a given function, NULL if none does. Used to find a command
// whatever it has been renamed to
static const command_t* command_running (void (*run)(char* arguments)) {
    for (size_t i = 0; i < command_count; i++) {
        if (commands[i].run == run) {
            return &commands[i];
        }
    }
    return NULL;
}

// The name of a command in capitals, found by the function it runs: command_name(cli_decrypt)
// gives "DEC". Messages that mention another command use this, so renaming a command in the
// table renames it in every message too.
// The name is written into one of a few small buffers, used in turn, so up to four names can
// be used in one prtf ("QUE adds one, GO runs them") without overwriting each other
const char* command_name (void (*run)(char* arguments)) {
    static char names[4][16];
    static int next = 0;
    const command_t* command = command_running(run);
    // only if the function was taken out of the table: a "?" in a message beats a crash
    if (command == NULL) {
        return "?";
    }
    // take the next buffer, going back to the first after the fourth
    char* name = names[next];
    next = (next + 1) % 4;
    scpy(name, command->name, sizeof(names[0]));
    strupr(name);
    return name;
}

// the same, found by the function's name instead: "cli_list" -> "LA" (see t_int.h)
const char* cli_command_called (const char* function) {
    for (size_t i = 0; i < command_count; i++) {
        if (seq(commands[i].function, function)) {
            return command_name(commands[i].run);
        }
    }
    return NULL;
}

// how wide the first column of HELP is ("DEL <NAME>" padded out to this many characters)
#define HELP_COLUMN 15

// HELP: one line for every command, in table order, then a line about apps
void cli_help (char* arguments) {
    (void)arguments;
    prtf("> %s:\n", command_name(cli_help));

    for (size_t i = 0; i < command_count; i++) {
        char usage[64];
        command_usage(&commands[i], usage, sizeof(usage));
        // "%-*s" pads the usage out to HELP_COLUMN characters, so the descriptions line up
        prtf("%-*s - %s\n", HELP_COLUMN, usage, commands[i].description);
    }
    // apps aren't in the table, so they get a line of their own at the end
    prtf("%-*s - %s\n", HELP_COLUMN, "<APP> [ARGS]", "RUN AN AERLANG PROGRAM, e.g. HELLO (/bin/hello.aer)");
}

// find a command in the table by name, ignoring upper / lower case. NULL if there is none
const command_t* find_command (const char* name) {
    for (size_t i = 0; i < command_count; i++) {
        if (strcasecmp(name, commands[i].name) == 0) {
            return &commands[i];
        }
    }
    return NULL;
}


// =================================================================================================
//  Tab completion
// =================================================================================================
//
// Like a Linux shell: press Tab part way through a word and the terminal finishes it.
//
//     the first word     commands (DEL, PLY...), apps in /bin/, and .aer programs here
//     any later word     files and folders, here ("no" -> "notes.txt") or along a path
//                        ("/bin/he" -> "/bin/hello.aer", "../"...). After SD, only folders
//
// What happens next depends on how many names fit what was typed:
//
//     one                it is filled in, then a space (or a '/' after a folder, so the
//                        next Tab carries on inside it)
//     several            filled in as far as they all agree: "re" -> "rea" for read / reader
//     several, and they
//     agree no further   they are listed under the line, then the prompt comes back
//
// Matching ignores upper / lower case, like the commands and the FAT file system do.

#define COMPLETE_MAX 128 // the most names gathered for one press of Tab

static struct {
    const char* typed;       // the part of the name typed so far
    size_t typed_length;
    bool folders_only;       // completing the folder after SD
    char names[COMPLETE_MAX][FS_NAME_MAX + 1]; // the names that fit, folders ending in '/'
    int count;
} completion;

// add a name that fits what was typed (unless it is already there: an app in /bin/ and a
// program here can share a name)
static void add_completion (const char* name, size_t length) {
    // the list is full, or the name wouldn't fit in it: quietly leave it out
    if (completion.count == COMPLETE_MAX || length > FS_NAME_MAX) {
        return;
    }
    if (strncasecmp(name, completion.typed, completion.typed_length) != 0) {
        return; // doesn't start with what was typed
    }
    // already in the list?
    for (int i = 0; i < completion.count; i++) {
        bool same = slen(completion.names[i]) == length
                    && strncasecmp(completion.names[i], name, length) == 0;
        if (same) {
            return;
        }
    }
    // memcpy and our own '\0', rather than scpy, because `name` may carry on past `length`
    // (a program's ".aer" is left off that way, see add_completion_program)
    memcpy(completion.names[completion.count], name, length);
    completion.names[completion.count][length] = '\0';
    completion.count++;
}

// fs_list() calls this for each entry while completing a file or folder.
// Folders get a '/' on the end
static void add_completion_entry (const char* name, bool is_dir, uint32_t size, void* context) {
    (void)size;
    (void)context;
    if (completion.folders_only && !is_dir) {
        return;
    }
    // room for the longest name, a '/' and the '\0'
    char full_name[FS_NAME_MAX + 2];
    sfmt(full_name, sizeof(full_name), "%s%s", name, is_dir ? "/" : "");
    add_completion(full_name, slen(full_name));
}

// fs_list() calls this for each entry while completing a command: only programs count, and
// they are offered without their ".aer" ("hello.aer" -> "hello")
static void add_completion_program (const char* name, bool is_dir, uint32_t size, void* context) {
    (void)size;
    (void)context;
    size_t length = slen(name);
    if (!is_dir && aer_is_program_name(name, length)) {
        add_completion(name, length - 4); // 4 is the length of ".aer"
    }
}

// the first word: every command, and every program in the app folders (/bin/, /bin/games/)
// and in the current folder
static void gather_commands (const char* word) {
    completion.typed = word;
    completion.typed_length = slen(word);

    // add_completion skips anything that doesn't start with `word`, so everything can be
    // offered to it and only the names that fit are kept
    for (size_t i = 0; i < command_count; i++) {
        add_completion(commands[i].name, slen(commands[i].name));
    }
    for (int f = 0; aer_app_folders[f] != NULL; f++) {
        fs_list(aer_app_folders[f], add_completion_program, NULL);
    }
    fs_list(cd, add_completion_program, NULL);
}

// a later word: the files and folders in the folder the word points into.
//     "docs/no"  ->  look in "docs/" for names starting with "no"
//     "no"       ->  look in the current folder
// `command` is the line's first word, to spot SD (which only takes folders)
static void gather_files (const char* word, const char* command) {
    const char* last_slash = strrchr(word, '/');
    char folder[FS_PATH_MAX];

    if (last_slash != NULL) {
        // the part up to and including the last '/' is the folder to look in
        char typed_folder[FS_PATH_MAX];
        size_t folder_length = (size_t)(last_slash - word) + 1;
        if (folder_length >= sizeof(typed_folder)) {
            return; // too long to be a folder's path, so nothing can fit
        }
        memcpy(typed_folder, word, folder_length);
        typed_folder[folder_length] = '\0';
        if (!fs_resolve_dir(typed_folder, folder, sizeof(folder))) {
            return; // no such folder
        }
        completion.typed = last_slash + 1; // only the part after the '/' is matched against names
    } else {
        scpy(folder, cd, sizeof(folder));
        completion.typed = word;
    }
    completion.typed_length = slen(completion.typed);

    // SD only takes folders, so after it only folders are offered
    const command_t* change_folder = command_running(cli_set_directory);
    completion.folders_only = change_folder != NULL && strcasecmp(command, change_folder->name) == 0;
    fs_list(folder, add_completion_entry, NULL);

    // /src/ is built in rather than kept on the disk, so listing "/" doesn't include it
    if (seq(folder, "/")) {
        add_completion_entry("src", true, 0, NULL);
    }
}

// how many characters all the gathered names have in common (they all start with what was
// typed, so at least that many)
static size_t shared_length (void) {
    // start with the whole of the first name, and cut it down to what each other name agrees
    // with, letter by letter (upper or lower case alike). The typed part is skipped, as every
    // name already matches it
    size_t shared = slen(completion.names[0]);
    for (int i = 1; i < completion.count; i++) {
        const char* name = completion.names[i];
        size_t same = completion.typed_length;
        while (same < shared && name[same] != '\0'
               && tolower((unsigned char)name[same]) == tolower((unsigned char)completion.names[0][same])) {
            same++;
        }
        shared = same;
    }
    return shared;
}

// list the gathered names in columns under the line, then print the prompt and the line again
static void list_completions (const char* line) {
    // every column is as wide as the longest name plus 2 spaces, and as many columns as fit
    // across the screen go on each row (at least one, however long the name)
    size_t widest = 0;
    for (int i = 0; i < completion.count; i++) {
        size_t width = slen(completion.names[i]);
        if (width > widest) {
            widest = width;
        }
    }
    int column_width = (int)widest + 2;
    int per_row = COLS / column_width;
    if (per_row < 1) {
        per_row = 1;
    }

    // down a line from what was typed, then the names. The last name in a row (or of all)
    // ends the line instead of being padded
    prtf("\n");
    for (int i = 0; i < completion.count; i++) {
        bool end_of_row = (i + 1) % per_row == 0 || i + 1 == completion.count;
        if (end_of_row) {
            prtf("%s\n", completion.names[i]);
        } else {
            prtf("%-*s", column_width, completion.names[i]); // padded out to the column width
        }
    }
    // the cursor is under the list now, so print the prompt and the line again there, for
    // typing to carry on
    cli_prompt();
    prtf("%s", line);
}

// add characters to the end of the line, showing them as they go. Returns the new length
static int type_into_line (char* line, int length, int size, const char* add, size_t add_length) {
    // stop one short of `size`, so the line can't overflow and there is room for its '\0'
    for (size_t i = 0; i < add_length && length < size - 1; i++) {
        line[length] = add[i];
        length++;
        console_putchar(add[i]);
    }
    line[length] = '\0';
    return length;
}

// what Tab does (the line editor in drivers/input/IO.c calls it). Given the line so far,
// finish the word at the end of it if it can. Returns the line's new length
static int cli_complete (char* line, int length, int size) {
    // the word being finished starts after the last space
    int word_start = length;
    while (word_start > 0 && line[word_start - 1] != ' ') {
        word_start--;
    }
    const char* word = line + word_start;

    // is it the first word on the line (only spaces before it)?
    bool first_word = true;
    for (int i = 0; i < word_start; i++) {
        if (line[i] != ' ') {
            first_word = false;
        }
    }

    // start each press of Tab with an empty list, then gather the names that fit
    completion.count = 0;
    completion.folders_only = false;

    if (first_word) {
        gather_commands(word);
    } else {
        // copy out the line's first word, the command, to see whether it is SD
        char command[16];
        int from = 0;
        while (line[from] == ' ') {
            from++;
        }
        int to = 0;
        while (line[from] != '\0' && line[from] != ' ' && to < (int)sizeof(command) - 1) {
            command[to++] = line[from++];
        }
        command[to] = '\0';
        gather_files(word, command);
    }

    if (completion.count == 0) {
        return length; // nothing starts like that
    }

    // can anything be filled in? Yes if the names agree on more than what was typed
    size_t shared = shared_length();
    bool can_add_more = shared > completion.typed_length;

    if (can_add_more || completion.count == 1) {
        // fill in the part they all share
        const char* first = completion.names[0];
        length = type_into_line(line, length, size, first + completion.typed_length, shared - completion.typed_length);

        // a single name is finished, so a space goes after it, ready for the next word. Not
        // after a folder though: the next Tab carries on inside it
        bool is_folder = first[slen(first) - 1] == '/';
        if (completion.count == 1 && !is_folder) {
            length = type_into_line(line, length, size, " ", 1);
        }
        return length;
    }

    // several names, and nothing more they agree on: show them all
    list_completions(line);
    return length;
}


// =================================================================================================
//  Running a line
// =================================================================================================

// the most code that can be typed in one go, over several lines (see run_code)
#define CODE_MAX 1024

// AerLang typed at the prompt (lang/aer_interp.c, "The shell"). A line that opens a block,
// like "for f in la() do", carries on: more lines are read, with "..." as their prompt, until
// every block has its end. An empty line stops waiting (and the code reports its missing end)
static void run_code (const char* first_line) {
    char code[CODE_MAX];
    scpy(code, first_line, sizeof(code));

    // aer_open_blocks counts the blocks opened ("do", "then"...) that have no "end" yet
    while (aer_open_blocks(code) > 0) {
        colour(DARK_GRAY, BLACK);
        prtf("   ... ");
        colour(BRIGHT, BLACK);
        // NULL: no Tab completion here, it only knows about commands and files
        char more[CLI_INPUT_MAX];
        GetLine(more, sizeof(more), NULL);
        if (trim(more)[0] == '\0') {
            break;
        }
        // each line joins the code on a line of its own, as it was typed. scat stops at
        // CODE_MAX, so a very long block is cut short rather than running past the end
        scat(code, "\n", sizeof(code));
        scat(code, more, sizeof(code));
    }
    aer_shell_run(code);
}

// ---- "Did you mean DEL?" -------------------------------------------------------------------------

// the closest name to what was typed, found so far
typedef struct {
    const char* typed;
    char best[FS_NAME_MAX];
    bool best_is_command;   // commands are shown in capitals, apps as they are
    int distance;           // how far best is from typed (see sdist)
} suggestion_t;

// try one more name: if it is closer to what was typed than the best so far, it becomes the
// best. A tie keeps the earlier one, so commands (tried first) win over apps
static void consider (suggestion_t* suggestion, const char* name, bool is_command) {
    int distance = sdist(name, suggestion->typed);
    if (distance < suggestion->distance) {
        suggestion->distance = distance;
        scpy(suggestion->best, name, sizeof(suggestion->best));
        suggestion->best_is_command = is_command;
    }
}

// fs_list() calls this for each file in the app folders (/bin/, /bin/games/): an app is a name
// to consider too, without its ".aer" ("hello.aer" -> "hello"). `context` is the suggestion_t
static void consider_app (const char* name, bool is_dir, uint32_t size, void* context) {
    (void)size;
    // only programs, and only ones whose name (less ".aer") fits in `app` with its '\0'
    size_t length = slen(name);
    if (is_dir || !aer_is_program_name(name, length) || length - 4 >= FS_NAME_MAX) {
        return;
    }
    char app[FS_NAME_MAX];
    memcpy(app, name, length - 4);
    app[length - 4] = '\0';
    consider(context, app, false);
}

// The first word isn't a command, an app or AerLang: say so, and suggest the command or app it
// is most like, if one is close (one letter out for a short name of 3 letters or fewer, two
// for a longer one). "dle" -> "Did you mean DEL?"
static void unknown_command (const char* word) {
    // the best so far starts just out of reach (allowed + 1), so only a name within `allowed`
    // can beat it. If none does, best stays "" and there is no suggestion
    int allowed = slen(word) <= 3 ? 1 : 2;
    suggestion_t suggestion = { word, "", false, allowed + 1 };
    for (size_t i = 0; i < command_count; i++) {
        consider(&suggestion, commands[i].name, true);
    }
    for (int f = 0; aer_app_folders[f] != NULL; f++) {
        fs_list(aer_app_folders[f], consider_app, &suggestion);
    }

    if (suggestion.best[0] == '\0') {
        explain_say(PROBLEM_UNKNOWN_COMMAND, word, "Unrecognised command: %s", word);
        return;
    }
    // commands are shown in capitals like everywhere else, apps as their files are named
    if (suggestion.best_is_command) {
        strupr(suggestion.best);
    }
    explain_say(PROBLEM_UNKNOWN_COMMAND, word, "Unrecognised command: %s. Did you mean %s?",
                word, suggestion.best);
}

// ---- running a line -------------------------------------------------------------------------------

// run one line the user typed. The first word decides what it is, in this order:
//     a command         "mkf notes.txt hello there"  ->  MKF, with "notes.txt hello there"
//     an app            "hello"                      ->  /bin/hello.aer
//     AerLang           "let x = 2", "x * 10"        ->  run in the shell (lang/aer_interp.c)
// and anything else is an unknown command
void HandleInput (char* in) {
    // drop the spaces at both ends, so "  la  " works too
    char* line = trim(in);
    if (line[0] == '\0') {
        return; // an empty line: nothing to do
    }

    // a copy of the whole line, in case it is AerLang: splitting off the first word cuts it up
    char whole_line[CLI_INPUT_MAX];
    scpy(whole_line, line, sizeof(whole_line));

    char* arguments;
    char* command_name = split_first_word(line, &arguments);

    // remember how many full screen clears there have been, to compare once the command is done
    unsigned int clears_before = vga_clear_count;

    // try each kind in turn. running_command is set only while a command runs, for show_usage
    const command_t* command = find_command(command_name);
    if (command != NULL) {
        running_command = command;
        command->run(arguments);
        running_command = NULL;
    } else if (run_program(command_name, arguments)) {
        // it was an app, e.g. "hello" ran /bin/hello.aer
    } else if (aer_shell_wants(whole_line)) {
        run_code(whole_line);
    } else {
        unknown_command(command_name);
    }

    // A program drew over the whole screen (the editor, the error screen...): bring the terminal
    // back. Only clear() counts up vga_clear_count, the console's own clearing (CLS) doesn't
    if (vga_clear_count != clears_before) {
        cli_rend_main();
    }
}

// run a line as if it had been typed, but keep what it prints in `out` instead of showing it.
// AerLang's cmd() uses this, so a program can run a command and look at what it said.
// While it runs the keyboard counts as unplugged, so a command that asks a question gets no answer
void cli_capture (const char* line, char* out, size_t size) {
    char copy[CLI_INPUT_MAX];
    scpy(copy, line, sizeof(copy)); // HandleInput cuts the line up, and `line` is the caller's
    // from here until console_capture_end, everything printed goes into `out`
    console_capture_begin(out, size);
    HandleInput(copy);
    console_capture_end();
}

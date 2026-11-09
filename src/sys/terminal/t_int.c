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
    /bin/hello.aer).

    ADDING OR RENAMING A COMMAND

    Everything about a command's name comes from that one table: HELP, Tab completion and the
    "Usage:" messages all read it. So to rename a command, change its name in the table and
    nothing else. To add one, write a function like this and add a line to the table:

        static void cli_hello (char* arguments) {
            prtf("Hello, %s!\n", arguments);
        }

    `arguments` is everything after the command, with the spaces at both ends removed. It is
    an empty string ("") when nothing was typed after the command, never NULL.

    FULL SCREEN PROGRAMS

    Some programs take over the whole screen (the editor, the picture viewer, the red error
    screen). They all start with clear(), which counts up vga_clear_count. HandleInput() checks
    that count before and after each command, and redraws the banner if it changed, so the
    terminal always comes back looking right.

    The commands below are in roughly the order HELP lists them: encrypted files, files and
    folders, the system, sound, apps and AerLang, the queue, then accounts.
*/

#include "t_int.h"
#include <stdint.h>
#include <stddef.h>
#include "../../lib/primary_definitions.h"
#include "../../drivers/video/VGA_linear.h"
#include "../../drivers/input/IO.h"
#include "../../drivers/audio/tone.h"
#include "../../drivers/storage/ata.h"
#include "../sys_clck.h"
#include "../fat.h"
#include "../f_sys.h"
#include "../boot.h"
#include "../power.h"
#include "../p_man.h"
#include "../../lang/aerlang.h"
#include "editor.h"
#include "../../drivers/video/bga.h"
#include "../../lib/crypto.h"
#include "../accounts/auth.h"
#include "../fcrypt.h"
#include "../sound.h"
#include "../ui.h"

// the longest line the user can type, counting the '\0' that ends it
#define CLI_INPUT_MAX 128

// the longest password the terminal asks for, counting the '\0'
#define CLI_PASSWORD_MAX 64

// true once the terminal has started. compile() (lib/aer_comp.c) checks it so it never starts
// a second terminal while this one is running
bool cli_active = false;

// set to false by the logout command, which sends the terminal back to the login screen
static bool cli_logged_in = false;

// the line the user is typing, re-used for every command
static char cli_input[CLI_INPUT_MAX];


// =================================================================================================
//  The command table, part 1: its shape
// =================================================================================================
//
// The table itself is near the bottom of the file, after all the functions it points to. Its
// shape comes first, so the usage messages up here can use it.
// (C reads a file from top to bottom, so anything used before it is written out in full has
// to be declared first, like the two functions at the end of this block.)

typedef struct {
    const char* name;          // what the user types, in lower case: "del" (DEL and Del work too)
    const char* arguments;     // what goes after it, for HELP and "Usage:": "<NAME>", or "" for nothing
    const char* description;   // the line HELP shows for it
    void (*run)(char* arguments);
} command_t;

// the command that is running right now, so show_usage() knows which one to explain
static const command_t* running_command = NULL;

static void cli_help (char* arguments);
static const char* help_command_name (void); // "HELP", or whatever it has been renamed to
static int cli_complete (char* line, int length, int size);


// =================================================================================================
//  Small helpers used by many commands
// =================================================================================================

// write a command's name in capitals followed by its arguments, e.g. "DEL <NAME>", into out
static void command_usage (const command_t* command, char* out, size_t size) {
    size_t i = 0;
    while (command->name[i] != '\0' && i < size - 1) {
        out[i] = (char)toupper((unsigned char)command->name[i]);
        i++;
    }
    out[i] = '\0';

    if (command->arguments[0] != '\0') {
        scat(out, " ", size);
        scat(out, command->arguments, size);
    }
}

// print "Usage: DEL <NAME>" for the command that is running, plus an optional hint after it
// (hint may be NULL). Every command uses this when it was typed without what it needs
static void show_usage (const char* hint) {
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

// print some text, adding a newline at the end unless it already ends with one, so the
// prompt after it always starts on a fresh line
static void print_text_block (const char* text, size_t length) {
    prtf("%s", text);
    bool ends_with_newline = length > 0 && text[length - 1] == '\n';
    if (!ends_with_newline) {
        prtf("\n");
    }
}

// ask a question and read the answer without showing it (each key shows as *)
static void ask_secret (const char* question, char* answer, int size) {
    prtf("%s", question);
    GetSecret(answer, size);
}

// ask "(y/n)" and wait for one key. Prints the answer so it stays on the screen
static bool ask_yes_no (void) {
    int key = getk();
    bool yes = key == 'y' || key == 'Y';
    prtf("%c\n", yes ? 'y' : 'n');
    return yes;
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
// This loop only ends if cli_active goes false, which nothing does yet: REBOOT and SHUTDOWN
// never come back, and logging out only ends the inner loop, back to the login screen
int cli_init (int ask_login) {
    cli_active = true;

    while (cli_active) {
        if (ask_login) {
            auth_login(); // comes back once someone has logged in
        }
        cli_logged_in = true;
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
    prtf("AerOS - Version " AEROS_VERSION "\n");
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
    prtf("Type %s for a list of commands, or DOCS to read the manual.\n\n", help_command_name());

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
//  Encrypted files (how the encryption works: sys/fcrypt.h)
// =================================================================================================

// commands that would break an encrypted file (editing it, adding text to it) call this first.
// Returns true, after saying why, if the file is encrypted
static bool refuse_if_encrypted (const char* path) {
    if (!fcrypt_file_is_encrypted(path)) {
        return false;
    }
    prtf("%s is encrypted, DECRYPT it first.\n", path);
    return true;
}

// RD on an encrypted file: ask for the password and show what is inside, leaving the file
// encrypted on the disk
static void read_encrypted_file (const char* path) {
    char password[CLI_PASSWORD_MAX];
    ask_secret("Password: ", password, sizeof(password));

    uint8_t* contents;
    uint32_t length;
    ui_busy_begin("Checking the password"); // turning a password into a key takes a moment
    int result = fcrypt_read_file(path, password, &contents, &length);
    ui_busy_end();
    wipe(password, sizeof(password)); // never leave a password lying around in memory

    if (result != FCRYPT_OK) {
        prtf("Can't read %s: %s\n", path, fcrypt_strerror(result));
        return;
    }
    print_text_block((char*)contents, length);

    wipe(contents, length);
    free(contents);
}

// ENCRYPT <FILE>: lock a file with a password, asked for twice to catch typing mistakes
static void cli_encrypt (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage(NULL);
        return;
    }

    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    bool found = fs_split_path(arguments, folder, sizeof(folder), name, sizeof(name))
                 && fs_file_exists(folder, name);
    if (!found) {
        prtf("File not found: %s\n", arguments);
        return;
    }
    if (fcrypt_file_is_encrypted(arguments)) {
        prtf("%s is already encrypted.\n", arguments);
        return;
    }

    char password[CLI_PASSWORD_MAX];
    char again[CLI_PASSWORD_MAX];
    ask_secret("Password: ", password, sizeof(password));
    ask_secret("Again:    ", again, sizeof(again));

    if (scmp(password, again) != 0) {
        prtf("The passwords don't match, nothing was changed.\n");
    } else {
        ui_busy_begin("Encrypting");
        int result = fcrypt_encrypt_file(arguments, password);
        ui_busy_end();

        if (result == FCRYPT_OK) {
            prtf("Encrypted %s. Don't lose the password, there is no way back without it.\n", arguments);
        } else {
            prtf("Couldn't encrypt %s: %s\n", arguments, fcrypt_strerror(result));
        }
    }

    wipe(password, sizeof(password));
    wipe(again, sizeof(again));
}

// DECRYPT <FILE>: turn an encrypted file back into a normal one
static void cli_decrypt (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage(NULL);
        return;
    }
    if (!fcrypt_file_is_encrypted(arguments)) {
        prtf("%s isn't an encrypted file.\n", arguments);
        return;
    }

    char password[CLI_PASSWORD_MAX];
    ask_secret("Password: ", password, sizeof(password));

    ui_busy_begin("Decrypting");
    int result = fcrypt_decrypt_file(arguments, password);
    ui_busy_end();
    wipe(password, sizeof(password));

    if (result == FCRYPT_OK) {
        prtf("Decrypted %s.\n", arguments);
    } else {
        prtf("Couldn't decrypt %s: %s\n", arguments, fcrypt_strerror(result));
    }
}


// =================================================================================================
//  Files and folders
// =================================================================================================

// split "notes.txt some text here" into the first word ("notes.txt") and the rest
// ("some text here", with its spaces kept). The line is cut in place. *rest is "" if there was
// nothing after the first word
static char* split_first_word (char* line, char** rest) {
    static char nothing[1] = "";
    char* words[2] = { line, NULL };
    int word_count = split(line, ' ', words, 2);

    if (word_count > 1) {
        *rest = trim(words[1]);
    } else {
        *rest = nothing;
    }
    return words[0];
}

// LA: list the files and folders in the current folder
static void cli_list (char* arguments) {
    (void)arguments; // LA takes no arguments, anything typed after it is ignored
    fs_print();
}

// SD <DIR>: set (change) the current folder. The path can be absolute ("/sys") or relative
// ("cache", "..", "."), fs_set_cd() works that out
static void cli_set_directory (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage("e.g. SD /sys, SD .., SD /");
        return;
    }
    if (fs_set_cd(arguments) != 0) {
        prtf("Directory not found: %s\n", arguments);
    }
}

// RD <FILE>: print what is in a file (asking for the password first if it is encrypted)
static void cli_read_file (char* arguments) {
    // fs_split_path turns "notes.txt" into the current folder + "notes.txt", and
    // "/sys/conf.aer" into "/sys/" + "conf.aer". It fails for an empty or invalid name
    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (!fs_split_path(arguments, folder, sizeof(folder), name, sizeof(name))) {
        show_usage(NULL);
        return;
    }

    if (fcrypt_file_is_encrypted(arguments)) {
        read_encrypted_file(arguments);
        return;
    }

    // fs_read_f gives back its own copy of the contents, which has to be freed afterwards
    char* contents = fs_read_f(folder, name);
    if (contents == NULL) {
        prtf("File not found: %s\n", arguments);
        return;
    }
    print_text_block(contents, slen(contents));
    free(contents);
}

// MKF <FILE> [TEXT]: make a new file, optionally with some text already in it
static void cli_make_file (char* arguments) {
    char* text;
    char* file = split_first_word(arguments, &text);

    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (!fs_split_path(file, folder, sizeof(folder), name, sizeof(name))) {
        show_usage(NULL);
        return;
    }

    // fails if the file is already there, the folder isn't, or the disk is full
    int result = fs_create_f(folder, name, text);
    if (result != FS_OK) {
        prtf("Could not create %s: %s\n", file, fs_strerror(result));
    }
}

// WR <FILE> <TEXT>: add a line of text to the end of a file that already exists
static void cli_write_file (char* arguments) {
    char* text;
    char* file = split_first_word(arguments, &text);

    // unlike MKF, both the file and the text are needed
    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (text[0] == '\0' || !fs_split_path(file, folder, sizeof(folder), name, sizeof(name))) {
        show_usage(NULL);
        return;
    }
    if (refuse_if_encrypted(file)) {
        return; // added text would break the file's tag, and it couldn't be decrypted any more
    }

    // read the file first: to check it is there, and to see whether it is empty
    char* existing = fs_read_f(folder, name);
    if (existing == NULL) {
        prtf("File not found: %s\n", file);
        return;
    }
    bool file_is_empty = existing[0] == '\0';
    free(existing);

    // the text goes on a new line, "\n<text>". An empty file gets just "<text>", so a file
    // never starts with a blank line
    size_t line_size = slen(text) + 2; // room for the '\n' and the '\0'
    char* line = malloc(line_size);
    if (line == NULL) {
        prtf("Out of memory.\n");
        return;
    }
    if (file_is_empty) {
        scpy(line, text, line_size);
    } else {
        scpy(line, "\n", line_size);
        scat(line, text, line_size);
    }

    bool append = true; // add to the end rather than replace what is there
    int result = fs_write_f(folder, name, line, append);
    free(line);

    if (result != FS_OK) {
        prtf("Could not write %s: %s\n", file, fs_strerror(result));
    }
}

// MKD <DIR>: make a new folder
static void cli_make_directory (char* arguments) {
    // fs_resolve_dir turns e.g. "cache" (typed in /sys/) into the full path "/sys/cache/".
    // "/" is turned away: it always exists, and has no name of its own
    char path[FS_PATH_MAX];
    bool valid = arguments[0] != '\0'
                 && fs_resolve_dir(arguments, path, sizeof(path))
                 && !seq(path, "/");
    if (!valid) {
        show_usage(NULL);
        return;
    }

    // the new folder's name is the last part of the path:
    //   path "/sys/cache/"  ->  parent "/sys/"  ->  what is left, "cache/"  ->  name "cache"
    char parent[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    fs_parent_path(path, parent, sizeof(parent));
    scpy(name, path + slen(parent), sizeof(name));
    name[slen(name) - 1] = '\0'; // drop the '/' on the end

    int result = fs_create_d(path, name);
    if (result != FS_OK) {
        prtf("Could not create %s: %s\n", path, fs_strerror(result));
    }
}

// ---- DEL ----------------------------------------------------------------------------------------

// fs_empty_dir() calls this after each file it deletes, to move DEL's progress bar along.
// `context` is whatever DEL handed to fs_empty_dir: here, a delete_progress_t
typedef struct {
    ui_progress_t bar;
    uint32_t files_done;
} delete_progress_t;

static void delete_progress_step (const char* name, void* context) {
    delete_progress_t* progress = context;
    progress->files_done++;
    ui_progress_update(&progress->bar, progress->files_done, name);
}

// fs_list() calls a function for every entry it finds. Here only the number of entries it
// returns is wanted, so this one does nothing
static void ignore_entry (const char* name, bool is_dir, uint32_t size, void* context) {
    (void)name;
    (void)is_dir;
    (void)size;
    (void)context;
}

// DEL on a file: a spinner turns while its clusters are freed (a big file has thousands),
// then "Deleted notes.txt." takes its place
static void delete_file (const char* typed, const char* folder, const char* name) {
    char label[FS_NAME_MAX + 16];
    sfmt(label, sizeof(label), "Deleting %s", name);

    ui_busy_begin(label);
    int result = fs_delete_f(folder, name);
    ui_busy_end();

    if (result == FS_OK) {
        prtf("Deleted %s.\n", name);
    } else {
        prtf("Can't delete %s: %s\n", typed, fs_strerror(result));
    }
}

// DEL on a folder: an empty one just goes. One with things in it is emptied first, after
// asking, with a progress bar
static void delete_folder (const char* path) {
    // the same rules fs_delete_d() has, but checked BEFORE anything is emptied:
    // never the top folder, and never the folder you are in (or one it is inside)
    if (seq(path, "/")) {
        prtf("The top folder, /, can't be deleted.\n");
        return;
    }
    bool standing_in_it = strncmp(cd, path, slen(path)) == 0;
    if (standing_in_it) {
        prtf("Can't delete %s: you are in it (SD somewhere else first).\n", path);
        return;
    }

    // look inside: how many files in all (for the question and the progress bar), and whether
    // there is anything at all (an empty sub folder is something, but not a file)
    ui_busy_begin("Looking inside the folder");
    int file_count = fs_count_files(path);
    int entry_count = fs_list(path, ignore_entry, NULL);
    ui_busy_end();

    if (file_count < 0) {
        file_count = 0;
    }

    if (entry_count > 0) {
        prtf("%s has %d file(s) in it. Delete it and everything in it? (y/n) ", path, file_count);
        if (!ask_yes_no()) {
            prtf("Nothing was deleted.\n");
            return;
        }

        delete_progress_t progress = { { 0, 0, 0 }, 0 };
        uint32_t bar_total = file_count > 0 ? (uint32_t)file_count : 1;
        bool zero_files_first = false; // a normal delete, not the secure wipe
        ui_progress_begin(&progress.bar, bar_total);
        int deleted = fs_empty_dir(path, zero_files_first, delete_progress_step, &progress);
        ui_progress_end(&progress.bar);

        if (deleted < 0) {
            deleted = 0;
        }
        prtf("Deleted %d file(s).\n", deleted);
    }

    // the folder is empty now, so it can go
    int result = fs_delete_d(path);
    if (result == FS_OK) {
        prtf("Deleted %s.\n", path);
    } else {
        prtf("Can't delete %s: %s\n", path, fs_strerror(result));
    }
}

// DEL <NAME>: delete a file, or a folder and everything in it.
// A name could be either, so it is tried as a file first, then as a folder
static void cli_delete (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage(NULL);
        return;
    }

    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    bool is_file = fs_split_path(arguments, folder, sizeof(folder), name, sizeof(name))
                   && fs_file_exists(folder, name);
    if (is_file) {
        delete_file(arguments, folder, name);
        return;
    }

    char path[FS_PATH_MAX];
    bool is_folder = fs_resolve_dir(arguments, path, sizeof(path)) && fs_dir_exists(path);
    if (is_folder) {
        delete_folder(path);
        return;
    }

    prtf("Not found: %s\n", arguments);
}


// =================================================================================================
//  The system
// =================================================================================================

// ECHO <TEXT>: print the text back
static void cli_echo (char* arguments) {
    prtf("%s\n", arguments);
}

// CLS: clear the screen.
// This uses console_clear(), not clear(), so vga_clear_count doesn't change and HandleInput
// doesn't redraw the banner: you get an empty screen with just the prompt
static void cli_clear (char* arguments) {
    (void)arguments;
    console_clear();
}

// CLK: the date and time from the computer's battery backed clock (the CMOS clock).
// In an emulator this is usually the host computer's time, often in UTC
static void cli_clock (char* arguments) {
    (void)arguments;
    rtc_time_t now;
    rtc_read(&now);
    // day-month-year, the British way round
    prtf("System time: %02u-%02u-%04u %02u:%02u:%02u\n",
           now.day, now.month, now.year, now.hour, now.minute, now.second);
}

// TIME: how long AerOS has been running, counted by the system timer (1000 ticks a second)
static void cli_uptime (char* arguments) {
    (void)arguments;
    uint32_t milliseconds = clkuptms();
    uint32_t seconds = milliseconds / 1000;

    uint32_t hours = seconds / 3600;
    uint32_t minutes = (seconds / 60) % 60; // 0 to 59
    seconds = seconds % 60;                 // 0 to 59
    prtf("Time since boot: %uh %02um %02us (%u ms)\n", hours, minutes, seconds, milliseconds);
}

// PMAN: print the process stack (the terminal itself is in it, RUNNING)
static void cli_pman (char* arguments) {
    (void)arguments;
    prtf("> PMAN:\n");
    stack_print();
}

// MEM: how much memory and storage is in use
static void cli_memory (char* arguments) {
    (void)arguments;

    // the computer's memory is only known if GRUB told us about it (see sys/boot.c)
    if (boot_multiboot_valid) {
        uint32_t total_mb = (sys_mem_lower_kb + sys_mem_upper_kb) / 1024;
        prtf("System memory: %u KB lower, %u KB upper (%u MB total)\n",
               sys_mem_lower_kb, sys_mem_upper_kb, total_mb);
    }
    prtf("Kernel heap: %u / %u bytes used\n", (unsigned int)heap_used(), (unsigned int)heap_total());
    prtf("Processes: %d / %d\n", p_cnt, P_STACK_SIZE);

    if (fs_mode == FS_MODE_DISK) {
        // Work in sectors (512 bytes, so 2 sectors = 1 KB) rather than "cluster size in KB": a
        // cluster can be a single sector, half a KB, which would round down to 0 KB
        uint32_t free_kb = fat_free_clusters() * fat_vol.sectors_per_cluster / 2;
        uint32_t total_kb = fat_vol.cluster_count * fat_vol.sectors_per_cluster / 2;
        prtf("Disk: FAT%d \"%s\", %u KB free of %u KB\n", fat_vol.type, fat_vol.label, free_kb, total_kb);
    } else {
        // without a disk the limits are the sizes of the in-memory file and folder tables
        prtf("VFS: %d / %d files, %d / %d directories\n", f_c, FS_MAX_FILES, d_c, FS_MAX_DIRS);
    }
}

// DRV: what is plugged into each of the 4 IDE positions: 2 cables ("buses"), each with a
// "master" and a "slave" drive
static void cli_drives (char* arguments) {
    (void)arguments;
    static const char* const bus_names[2] = { "Primary", "Secondary" };

    for (int bus = 0; bus < 2; bus++) {
        for (int slave = 0; slave < 2; slave++) {
            char model[41]; // a drive's model name is up to 40 characters
            uint32_t sectors;
            int type = drive_probe_info(bus, slave, model, sizeof(model), &sectors);

            const char* position = slave ? "slave" : "master";
            prtf("%-9s %-6s : %s %s", bus_names[bus], position, drive_type_name(type), model);

            // only hard drives report a size. 2048 sectors of 512 bytes = 1 MB
            if (sectors > 0) {
                prtf(" (%u MB)", sectors / 2048);
            }
            // point out the drive AerOS keeps its files on
            bool mounted = fs_mode == FS_MODE_DISK && fat_vol.bus == bus && fat_vol.slave == slave;
            if (mounted) {
                prtf(" [mounted]");
            }
            prtf("\n");
        }
    }
}

// BEEP [HZ]: a beep on the PC speaker. atoi() gives 0 for nothing (or not a number), and
// beep(0) is the standard beep
static void cli_beep (char* arguments) {
    beep(atoi(arguments));
}

// REBOOT: restart the computer (never comes back)
static void cli_reboot (char* arguments) {
    (void)arguments;
    sys_reboot();
}

// SHUTDOWN: turn the computer off, or show "It is now safe to turn off your computer" if it
// can't be turned off by software (never comes back)
static void cli_shutdown (char* arguments) {
    (void)arguments;
    sys_shutdown();
}

// ---- RES ----------------------------------------------------------------------------------------

// Save the resolution for next time. GRUB reads it before AerOS starts (grub.cfg uses load_env)
// and asks the graphics card for that mode, which works on any PC.
// The file is a "GRUB environment block" in the disk's top folder, and GRUB is fussy about it:
// exactly 1024 bytes, a fixed first line, then "name=value" lines, padded out with '#'
static bool save_resolution (uint32_t width, uint32_t height) {
    if (fs_mode != FS_MODE_DISK) {
        return false; // nowhere to save it
    }

    char block[1025];
    int used = sfmt(block, sizeof(block), "# GRUB Environment Block\naeros_mode=%ux%ux32\n", width, height);
    memset(block + used, '#', (size_t)(1024 - used));
    block[1024] = '\0';

    int result;
    if (fs_file_exists("/", "grubenv")) {
        result = fs_write_f("/", "grubenv", block, false);
    } else {
        result = fs_create_f("/", "grubenv", block);
    }
    return result >= 0;
}

// read "1280x720" into width and height, false if it isn't a size AerOS can use
static bool parse_resolution (const char* text, long* width, long* height) {
    char* end;
    *width = strtol(text, &end, 10);
    *height = 0;
    if (*end == 'x' || *end == 'X') {
        *height = strtol(end + 1, &end, 10);
    }

    bool nothing_after = *end == '\0';
    bool big_enough = *width >= 640 && *height >= 400;
    bool small_enough = *width <= 4096 && *height <= 4096;
    return nothing_after && big_enough && small_enough;
}

// RES [WxH]: show the screen resolution, or change it, e.g. RES 1280x720
static void cli_resolution (char* arguments) {
    vga_framebuffer_t screen;
    bool graphics = vga_framebuffer_info(&screen);

    // no size given: say what it is now
    if (arguments[0] == '\0') {
        if (graphics) {
            prtf("Screen: %ux%u, %u bits per pixel\n", screen.width, screen.height, (unsigned int)screen.bpp);
        } else {
            prtf("Screen: VGA text mode\n");
        }
        prtf("Change it with RES <width>x<height>, e.g. RES 1280x720\n");
        prtf("Common sizes: 640x480 800x600 1024x768 1280x720 1280x1024 1600x900 1920x1080\n");
        return;
    }

    long width, height;
    if (!parse_resolution(arguments, &width, &height)) {
        show_usage("at least 640x400, e.g. RES 1280x720");
        return;
    }

    // try both: changing it now (only some graphics cards can) and saving it for next time
    bool changed = bga_set_mode((uint32_t)width, (uint32_t)height);
    bool saved = save_resolution((uint32_t)width, (uint32_t)height);

    if (changed) {
        vga_framebuffer_info(&screen);
        prtf("The screen is now %ux%u.\n", screen.width, screen.height);
    }
    if (saved) {
        prtf("Saved: AerOS will start at %ldx%ld from now on.\n", width, height);
    } else if (changed) {
        prtf("Not saved for the next start, that needs an AEROS disk.\n");
    } else {
        prtf("This graphics card can't change resolution while AerOS is running,\n");
        prtf("and without an AEROS disk it can't be saved for the next start either.\n");
    }
}


// =================================================================================================
//  Sound
// =================================================================================================
//
// PLAY shows one status line. While the file is being read (and an MP3 decoded) it is a
// "Reading song.mp3" spinner, turned by busy() calls from the decoder. Once the sound card
// starts, it asks play_should_stop() every few milliseconds, and the line becomes
//
//     (=)  Playing song.mp3  0:12  (Esc stops it)

static struct {
    const char* file;
    bool playing;            // has the sound card started yet?
    uint8_t row;             // the screen row of the status line
    uint32_t started_ms;     // when it started playing
    uint32_t last_drawn_ms;  // when the status line was last drawn
} play_status;

// the sound card calls this while it plays: redraw the status line (10 times a second at
// most), and say whether to stop (Esc or Ctrl+C was pressed)
static bool play_should_stop (void) {
    uint32_t now = clkuptms();

    // the first call: the "Reading" spinner's line becomes the "Playing" line
    if (!play_status.playing) {
        ui_busy_end();
        uint8_t column;
        console_get_cursor(&column, &play_status.row);
        prtf("\n");
        play_status.playing = true;
        play_status.started_ms = now;
        play_status.last_drawn_ms = 0;
    }

    bool time_to_redraw = play_status.last_drawn_ms == 0 || now - play_status.last_drawn_ms >= 100;
    if (time_to_redraw) {
        play_status.last_drawn_ms = now;
        uint32_t seconds = (now - play_status.started_ms) / 1000;

        char line[COLS + 1];
        sfmt(line, sizeof(line), "Playing %s  %u:%02u  (Esc stops it)",
                 play_status.file, seconds / 60, seconds % 60);

        int spinner_style = 4; // the "bounce" spinner, see sys/ui.h
        int spinner_width = ui_spinner(2, play_status.row, spinner_style, LIGHT_GREEN);
        puts((uint8_t)(3 + spinner_width), play_status.row, BRIGHT, console_bg, line);
    }

    if (kb_take_break()) {
        return true; // Ctrl+C
    }
    return kb_haschar() && getk() == KEY_ESCAPE;
}

// PLAY <FILE>: play a WAV or MP3 file on the sound card
static void cli_play (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage("a .wav or .mp3 file");
        return;
    }

    play_status.file = arguments;
    play_status.playing = false;

    char label[COLS];
    sfmt(label, sizeof(label), "Reading %s", arguments);
    ui_busy_begin(label);

    int result = sound_play_file(arguments, play_should_stop);

    // it never started playing (a missing file, or not a sound file): take the spinner away
    if (!play_status.playing) {
        ui_busy_end();
    }
    if (result != SOUND_OK) {
        prtf("Can't play %s: %s\n", arguments, sound_strerror(result));
    }
}


// =================================================================================================
//  Apps and AerLang
// =================================================================================================

// AER: the interactive AerLang prompt
static void cli_aer (char* arguments) {
    (void)arguments;
    aer_repl();
}

// EDIT <FILE>: open a file in the text editor (sys/terminal/editor.c)
static void cli_edit (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage("e.g. EDIT hello.aer");
        return;
    }
    if (refuse_if_encrypted(arguments)) {
        return;
    }
    editor_open(arguments);
}

// DOCS [TOPIC]: open the manual in the reader app. "docs sound" opens /docs/sound.md
static void cli_docs (char* arguments) {
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

// run some AerLang code as a process of its own, then remove the process again.
// Returns false if the process stack is full
static bool run_as_process (const char* code) {
    int pid = p_create(code);
    if (pid < 0) {
        prtf("The process stack is full.\n");
        return false;
    }
    // The terminal is itself a RUNNING process, so p_exec() runs just this new one (it knows
    // not to start the terminal a second time)
    p_exec(pid);
    p_destroy(pid);
    return true;
}

// RUN <CODE>: run a line of AerLang code as a new process, e.g. "run s:hello".
// Code that doesn't make sense shows the red error screen, and the terminal is redrawn after
static void cli_run (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage("e.g. RUN print_stack, RUN s:hi");
        return;
    }
    run_as_process(arguments);
}

// is there a program file with this name, in the current folder or (for a plain name) in /bin/?
static bool program_exists (const char* file) {
    char folder[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (!fs_split_path(file, folder, sizeof(folder), name, sizeof(name))) {
        return false;
    }
    if (fs_file_exists(folder, name)) {
        return true;
    }
    bool plain_name = strchr(file, '/') == NULL; // "hello", not "games/hello"
    return plain_name && fs_file_exists("/bin/", name);
}

// turn a program name and its arguments into the line its process runs:
//     "hello" + "a b"   ->   "hello.aer a b"
// (".aer" is only added if the name doesn't already end in it). False if there is no such program
static bool make_program_line (const char* program, const char* arguments, char* line, size_t size) {
    scpy(line, program, size);
    if (!aer_is_program_name(line, slen(line))) {
        scat(line, ".aer", size);
    }
    if (!program_exists(line)) {
        return false;
    }
    if (arguments[0] != '\0') {
        scat(line, " ", size);
        scat(line, arguments, size);
    }
    return true;
}

// a word that isn't a command may be an app: "hello" runs /bin/hello.aer.
// Returns false if there is no program by that name
static bool run_program (const char* program, const char* arguments) {
    char line[P_INST_MAX];
    if (!make_program_line(program, arguments, line, sizeof(line))) {
        return false;
    }
    run_as_process(line);
    return true;
}


// =================================================================================================
//  The queue: programs lined up to run one after another (see "The queue" in sys/p_man.h)
// =================================================================================================

// p_queue_each() calls this for each queued program, in the order they will run
static void print_queued_program (int position, const char* code, void* context) {
    (void)context;
    prtf("  %d. %s\n", position, code);
}

// QUEUE on its own: list what is queued
static void list_queue (void) {
    if (p_queue_count() == 0) {
        prtf("The queue is empty. QUEUE <program> adds one, GO runs them in order.\n");
        return;
    }
    prtf("Queued, in the order GO will run them:\n");
    p_queue_each(print_queued_program, NULL);
}

// QUEUE [PROGRAM]:
//     QUEUE hello a b     put hello.aer (with arguments "a b") at the end of the queue
//     QUEUE s:some text   queue a line of text to print, handy between programs
//     QUEUE               list the queue
//     QUEUE CLEAR         empty it
static void cli_queue (char* arguments) {
    if (arguments[0] == '\0') {
        list_queue();
        return;
    }
    if (strcasecmp(arguments, "clear") == 0) {
        prtf("Took %d program(s) out of the queue.\n", p_queue_clear());
        return;
    }

    char line[P_INST_MAX];
    bool is_message = strncmp(arguments, "s:", 2) == 0;
    if (is_message) {
        scpy(line, arguments, sizeof(line));
    } else {
        char* program_arguments;
        char* program = split_first_word(arguments, &program_arguments);
        if (!make_program_line(program, program_arguments, line, sizeof(line))) {
            prtf("There is no program called %s to queue.\n", program);
            return;
        }
    }

    int queued = p_queue_add(line);
    if (queued < 0) {
        prtf("The process stack is full.\n");
    } else {
        prtf("Queued %s (%d in the queue, GO runs them).\n", line, queued);
    }
}

// GO calls this before each queued program: a grey "[2/3] hello.aer" line
static void announce_queued_program (int number, int total, const char* code) {
    colour(DARK_GRAY, BLACK);
    prtf("[%d/%d] %s\n", number, total, code);
    colour(BRIGHT, BLACK);
}

// GO calls this after each queued program: Ctrl+C in one of them stops the rest too
static bool queue_should_stop (void) {
    return aer_take_stopped();
}

// GO: run everything in the queue, in the order it was queued
static void cli_go (char* arguments) {
    (void)arguments;
    if (p_queue_count() == 0) {
        prtf("The queue is empty. QUEUE <program> adds one.\n");
        return;
    }

    aer_take_stopped(); // forget a Ctrl+C from before GO started
    int ran = p_queue_run(announce_queued_program, queue_should_stop);
    int left = p_queue_count();

    if (left > 0) {
        prtf("Stopped the queue after %d program(s), %d still queued (GO carries on).\n", ran, left);
    } else {
        prtf("Ran %d program(s), the queue is empty.\n", ran);
    }
}


// =================================================================================================
//  Accounts (how passwords are kept: sys/accounts/auth.h)
// =================================================================================================

// the account commands need a disk to keep the accounts on. Without one, say so and return false
static bool accounts_available (void) {
    if (auth_required()) {
        return true;
    }
    prtf("Accounts need an AEROS disk, without one everyone is guest.\n");
    return false;
}

// ask for a new password twice. False, after saying why, if it was empty or they didn't match
static bool ask_new_password (char* password, int size) {
    char again[CLI_PASSWORD_MAX];
    ask_secret("New password: ", password, size);
    ask_secret("Again:        ", again, sizeof(again));

    bool empty = password[0] == '\0';
    bool match = scmp(password, again) == 0;
    wipe(again, sizeof(again));

    if (empty) {
        prtf("The passwords can't be empty, nothing was changed.\n");
        return false;
    }
    if (!match) {
        prtf("The passwords don't match, nothing was changed.\n");
        return false;
    }
    return true;
}

// before doing something drastic, ask for the logged in user's own password.
// True if it was right
static bool confirm_own_password (void) {
    char password[CLI_PASSWORD_MAX];
    ask_secret("Your password: ", password, sizeof(password));

    ui_busy_begin("Checking the password");
    bool right = auth_check(auth_user, password);
    ui_busy_end();
    wipe(password, sizeof(password));

    if (!right) {
        prtf("Wrong password.\n");
    }
    return right;
}

// WHOAMI: print who is logged in
static void cli_whoami (char* arguments) {
    (void)arguments;
    prtf("%s\n", auth_user);
}

// auth_list() calls this for every account
static void print_user (const char* name, void* context) {
    (void)context;
    if (seq(name, auth_user)) {
        prtf("%s  (you)\n", name);
    } else {
        prtf("%s\n", name);
    }
}

// USERS: list every account
static void cli_users (char* arguments) {
    (void)arguments;
    if (accounts_available()) {
        auth_list(print_user, NULL);
    }
}

// make a new account: <NAME>, then a password asked for twice
static void cli_add_user (char* arguments) {
    if (!accounts_available()) {
        return;
    }
    char* name = strlwr(arguments); // names are kept in lower case
    if (name[0] == '\0') {
        show_usage(NULL);
        return;
    }
    // check the name before asking for a password, not after
    if (!auth_valid_new_name(name)) {
        prtf("Can't use that name: %s\n", auth_strerror(AUTH_ERR_NAME));
        return;
    }
    if (auth_exists(name)) {
        prtf("%s already exists.\n", name);
        return;
    }

    char password[CLI_PASSWORD_MAX];
    if (ask_new_password(password, sizeof(password))) {
        ui_busy_begin("Saving the account"); // hashing the password takes a moment
        int result = auth_add(name, password);
        ui_busy_end();

        if (result == AUTH_OK) {
            char home[FS_PATH_MAX];
            if (auth_home(name, home, sizeof(home))) {
                prtf("Made the account %s, with the home folder %s.\n", name, home);
            } else {
                prtf("Made the account %s.\n", name);
            }
        } else {
            prtf("Couldn't make the account: %s\n", auth_strerror(result));
        }
    }
    wipe(password, sizeof(password));
}

// delete someone else's account (your own password is asked for first). Their home folder
// is left as it is
static void cli_delete_user (char* arguments) {
    if (!accounts_available()) {
        return;
    }
    char* name = strlwr(arguments);
    if (name[0] == '\0') {
        show_usage(NULL);
        return;
    }
    if (seq(name, auth_user)) {
        prtf("You can't delete the account you are logged in as.\n");
        return;
    }
    if (!auth_exists(name)) {
        prtf("There is no user called %s.\n", name);
        return;
    }
    if (!confirm_own_password()) {
        return;
    }

    if (auth_delete(name) == AUTH_OK) {
        prtf("Deleted %s.\n", name);
    } else {
        prtf("Couldn't delete %s.\n", name);
    }
}

// change your own password (the old one is asked for first)
static void cli_password (char* arguments) {
    (void)arguments;
    if (!accounts_available() || !confirm_own_password()) {
        return;
    }

    char password[CLI_PASSWORD_MAX];
    if (ask_new_password(password, sizeof(password))) {
        ui_busy_begin("Saving the password");
        int result = auth_set_password(auth_user, password);
        ui_busy_end();

        if (result == AUTH_OK) {
            prtf("Password changed.\n");
        } else {
            prtf("Couldn't save the new password.\n");
        }
    }
    wipe(password, sizeof(password));
}

// log out, back to the login screen
static void cli_logout (char* arguments) {
    (void)arguments;
    if (!accounts_available()) {
        return;
    }
    // the next person shouldn't be able to see what this one did: forget the commands for
    // the up arrow and the output for Page Up, and start them in the top folder
    ClearInputHistory();
    console_forget_scrollback();
    scpy(cd, "/", sizeof(cd));
    cli_logged_in = false; // ends the inner loop in cli_init()
}


// =================================================================================================
//  The command table, part 2: the commands
// =================================================================================================
//
// In the order HELP lists them. Names are matched without caring about upper or lower case.
// Columns: name, what goes after it, what it does, the function that runs it.

static const command_t commands[] = {
    { "hlp",      "",           "THIS COMMAND",                                               cli_help },
    { "la",       "",           "LIST ALL FILES AND DIRECTORIES IN THE CURRENT DIRECTORY",    cli_list },
    { "sd",       "<DIR>",      "SET THE CURRENT DIRECTORY (e.g. SD /sys, SD .., SD /)",      cli_set_directory },
    { "clk",      "",           "PRINTS THE SYSTEM TIME",                                     cli_clock },
    { "time",     "",           "PRINTS THE TIME SINCE BOOT",                                 cli_uptime },
    { "pman",     "",           "PRINTS THE PROCESS STACK",                                   cli_pman },
    { "queue",    "[PROGRAM]",  "QUEUE A PROGRAM FOR GO, OR LIST THE QUEUE (QUEUE CLEAR)",    cli_queue },
    { "go",       "",           "RUN EVERYTHING IN THE QUEUE, IN ORDER (CTRL+C STOPS IT)",    cli_go },
    { "run",      "<CODE>",     "RUN CODE AS A PROCESS (e.g. run print_stack, run s:hi)",     cli_run },
    { "rd",       "<FILE>",     "READ A FILE",                                                cli_read_file },
    { "mkf",      "<FILE> [T]", "MAKE A FILE, WITH OPTIONAL TEXT",                            cli_make_file },
    { "wr",       "<FILE> <T>", "ADD A LINE OF TEXT TO THE END OF A FILE",                    cli_write_file },
    { "mkd",      "<DIR>",      "MAKE A DIRECTORY",                                           cli_make_directory },
    { "del",      "<NAME>",     "DELETE A FILE, OR A DIRECTORY AND ALL IN IT (ASKS FIRST)",   cli_delete },
    { "echo",     "<TEXT>",     "PRINT TEXT",                                                 cli_echo },
    { "cls",      "",           "CLEAR THE SCREEN",                                           cli_clear },
    { "mem",      "",           "MEMORY USAGE",                                               cli_memory },
    { "drv",      "",           "LIST IDE / ATA DRIVES",                                      cli_drives },
    { "beep",     "[HZ]",       "PLAY A BEEP ON THE PC SPEAKER",                              cli_beep },
    { "play",     "<FILE>",     "PLAY A .WAV OR .MP3 FILE (ESC TO STOP)",                     cli_play },
    { "reboot",   "",           "RESTART THE COMPUTER",                                       cli_reboot },
    { "shutdown", "",           "TURN OFF THE COMPUTER",                                      cli_shutdown },
    { "res",      "[WxH]",      "SHOW OR CHANGE THE SCREEN RESOLUTION (e.g. RES 1280x720)",   cli_resolution },
    { "edit",     "<FILE>",     "EDIT A FILE (CTRL+S SAVE, CTRL+R RUN A .AER, CTRL+Q QUIT)",  cli_edit },
    { "aer",      "",           "THE AERLANG PROMPT, TYPE CODE TO RUN IT",                    cli_aer },
    { "docs",     "[TOPIC]",    "READ THE MANUAL (e.g. DOCS SOUND), READ <FILE> FOR ANY FILE", cli_docs },
    { "encrypt",  "<FILE>",     "LOCK A FILE WITH A PASSWORD (RD ASKS FOR IT TO SHOW THE FILE)", cli_encrypt },
    { "decrypt",  "<FILE>",     "UNLOCK AN ENCRYPTED FILE",                                   cli_decrypt },
    { "whoami",   "",           "PRINT WHO IS LOGGED IN",                                     cli_whoami },
    { "users",    "",           "LIST THE USER ACCOUNTS",                                     cli_users },
    { "newuser",  "<NAME>",     "MAKE A NEW USER ACCOUNT",                                    cli_add_user },
    { "deluser",  "<NAME>",     "DELETE A USER ACCOUNT",                                      cli_delete_user },
    { "pass",     "",           "CHANGE YOUR PASSWORD",                                       cli_password },
    { "out",      "",           "LOG OUT, BACK TO THE LOGIN SCREEN",                          cli_logout },
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

// the name of the help command in capitals, for the banner and HELP's own heading
static const char* help_command_name (void) {
    static char name[16];
    const command_t* help = command_running(cli_help);
    if (help == NULL) {
        return "HELP";
    }
    scpy(name, help->name, sizeof(name));
    strupr(name);
    return name;
}

// how wide the first column of HELP is ("DEL <NAME>" padded out to this many characters)
#define HELP_COLUMN 15

// HELP: one line for every command, in table order, then a line about apps
static void cli_help (char* arguments) {
    (void)arguments;
    prtf("> %s:\n", help_command_name());

    for (size_t i = 0; i < command_count; i++) {
        char usage[64];
        command_usage(&commands[i], usage, sizeof(usage));
        prtf("%-*s - %s\n", HELP_COLUMN, usage, commands[i].description);
    }
    prtf("%-*s - %s\n", HELP_COLUMN, "<APP> [ARGS]", "RUN AN AERLANG PROGRAM, e.g. HELLO (/bin/hello.aer)");
}

// find a command in the table by name, ignoring upper / lower case. NULL if there is none
static const command_t* find_command (const char* name) {
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
//     the first word     commands (DEL, PLAY...), apps in /bin/, and .aer programs here
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
    if (completion.count == COMPLETE_MAX || length > FS_NAME_MAX) {
        return;
    }
    if (strncasecmp(name, completion.typed, completion.typed_length) != 0) {
        return; // doesn't start with what was typed
    }
    for (int i = 0; i < completion.count; i++) {
        bool same = slen(completion.names[i]) == length
                    && strncasecmp(completion.names[i], name, length) == 0;
        if (same) {
            return;
        }
    }
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
        add_completion(name, length - 4);
    }
}

// the first word: every command, and every program in /bin/ and in the current folder
static void gather_commands (const char* word) {
    completion.typed = word;
    completion.typed_length = slen(word);

    for (size_t i = 0; i < command_count; i++) {
        add_completion(commands[i].name, slen(commands[i].name));
    }
    fs_list("/bin/", add_completion_program, NULL);
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
            return;
        }
        memcpy(typed_folder, word, folder_length);
        typed_folder[folder_length] = '\0';
        if (!fs_resolve_dir(typed_folder, folder, sizeof(folder))) {
            return; // no such folder
        }
        completion.typed = last_slash + 1;
    } else {
        scpy(folder, cd, sizeof(folder));
        completion.typed = word;
    }
    completion.typed_length = slen(completion.typed);

    // SD only takes folders, so after it only folders are offered
    const command_t* change_folder = command_running(cli_set_directory);
    completion.folders_only = change_folder != NULL && strcasecmp(command, change_folder->name) == 0;
    fs_list(folder, add_completion_entry, NULL);
}

// how many characters all the gathered names have in common (they all start with what was
// typed, so at least that many)
static size_t shared_length (void) {
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

    prtf("\n");
    for (int i = 0; i < completion.count; i++) {
        bool end_of_row = (i + 1) % per_row == 0 || i + 1 == completion.count;
        if (end_of_row) {
            prtf("%s\n", completion.names[i]);
        } else {
            prtf("%-*s", column_width, completion.names[i]); // padded out to the column width
        }
    }
    cli_prompt();
    prtf("%s", line);
}

// add characters to the end of the line, showing them as they go. Returns the new length
static int type_into_line (char* line, int length, int size, const char* add, size_t add_length) {
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

// run one line the user typed:
//     "  mkf notes.txt hello there "   ->   command "mkf", arguments "notes.txt hello there"
void HandleInput (char* in) {
    // drop the spaces at both ends, so "  la  " works too
    char* line = trim(in);
    if (line[0] == '\0') {
        return; // an empty line: nothing to do
    }

    char* arguments;
    char* command_name = split_first_word(line, &arguments);

    // remember how many full screen clears there have been, to compare once the command is done
    unsigned int clears_before = vga_clear_count;

    const command_t* command = find_command(command_name);
    if (command != NULL) {
        running_command = command;
        command->run(arguments);
        running_command = NULL;
    } else if (run_program(command_name, arguments)) {
        // it was an app, e.g. "hello" ran /bin/hello.aer
    } else {
        prtf("> %s\n", command_name);
        prtf("Unrecognised command, please try again.\n");
    }

    // A program drew over the whole screen (the editor, the error screen...): bring the terminal
    // back. Only clear() counts up vga_clear_count, the console's own clearing (CLS) doesn't
    if (vga_clear_count != clears_before) {
        cli_rend_main();
    }
}

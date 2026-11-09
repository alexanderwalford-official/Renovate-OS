// terminal interface

/*
    t_int.c - the AerOS terminal

    This is where the user spends their time once the boot has finished. kernel.c creates a
    process called "terminal", the compiler turns that into cli_init(1), and from then on
    the terminal loop below is the OS:

        while the terminal is up:
            auth_login()               the login screen (DISK mode only, sys/accounts/auth.h)
            cli_rend_main()            draw the banner
            until LOGOUT:
                cli_prompt()           e.g. "alex@0//sys/: "
                GetString()            read a line (the keyboard driver echoes it and keeps history)
                HandleInput()          find the command in cli_commands[] and run it

    The commands are grouped in this file roughly as HELP lists them: files, system, encrypted
    files (sys/fcrypt.h), sound (sys/sound.h), apps and AerLang, then accounts.

    Because the terminal is itself a RUNNING process, the RUN command (which runs a line of
    AerLang as a new process) works by nesting: the new process is run from inside the
    terminal's process, and the process manager knows not to start the terminal again.

    Programs started with RUN can take over the whole screen (the splash screen, the error
    screen, the boot options menu). They all draw with clear(), which counts up
    vga_clear_count, so HandleInput() can tell afterwards that the terminal was drawn over
    and redraw the banner.

    Adding a command: write a static cli_<name>(char* args) handler and add a line for it to
    cli_commands[], that is all HELP and HandleInput need.
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

// the longest line the user can type, including the terminating '\0'
#define CLI_INPUT_MAX 128

bool cli_active = false; // true from cli_init() onwards, compile() checks it before starting another terminal
static char cli_input[CLI_INPUT_MAX]; // the line being typed, re-used for every command

// Tab completion for the command line (defined after the command table, which it needs)
static int cli_complete (char* line, int length, int size);

// flash a block at the cursor position, times = number of flashes, lc = flashes already done
// each flash takes a second, and the OS waits while it happens. Returns 0
int flash_pointer (int times, int lc) {
    uint8_t x, y;
    console_get_cursor(&x, &y);
    // lc doubles as the loop counter, so we pick up wherever the caller left off
    for (; lc < times; lc++) {
        // a space with the colours swapped: the background becomes the text colour, a solid block
        putc(x, y, console_bg, console_fg, ' '); // solid block
        clck_hang(500);
        // and back to the normal colours, a blank cell
        putc(x, y, console_fg, console_bg, ' '); // empty
        clck_hang(500);
    }
    return 0;
}

// false once LOGOUT is typed, which sends the terminal back to the login screen
static bool cli_logged_in = false;

// start the terminal, ent = 1 asks who is using it first (ent = 0 skips the login, as "guest")
// this is the terminal loop. It only ends if cli_active goes false, which nothing does yet: the user
// leaves the terminal with REBOOT or SHUTDOWN, and those never return. LOGOUT ends the inner loop,
// and the outer one shows the login screen again
int cli_init (int ent) {
    cli_active = true;
    while (cli_active) {
        if (ent) {
            auth_login(); // returns once someone has logged in (straight away as guest without a disk)
        }
        cli_logged_in = true;
        cli_rend_main();
        while (cli_active && cli_logged_in) {
            cli_prompt();
            // waits for Enter, the line comes back without the newline (and is added to the up / down history)
            // Tab fills in commands and file names (cli_complete)
            // gotcha: with no keyboard it returns an empty line straight away, so the prompt just repeats
            GetLine(cli_input, sizeof(cli_input), cli_complete);
            HandleInput(cli_input);
        }
    }
    return 0;
}

// draw the terminal's start screen
// the coloured line tells the user where their files go:
//   green  DISK mode, a AEROS disk was mounted and files are saved to it
//   yellow DEMO mode, picked from the boot options, the disks were never looked at
//   red    VFS mode, no usable disk was found, files only live in memory until restart
// also used to bring the terminal back after a full screen program (see HandleInput). Returns 0
int cli_rend_main (void) {
    // console_clear() fills the screen with the current colours, so set them first
    console_set_color(WHITE, BLACK);
    console_clear();
    printf("AerOS - Version " AEROS_VERSION "\n");
    printf("Alexander Walford 2026\n\n");
    // demo mode always runs from memory, so DISK mode is checked first and only VFS mode needs telling apart
    if (fs_mode == FS_MODE_DISK) {
        console_set_color(LIGHT_GREEN, BLACK);
        printf("[ * ] You are in DISK mode, your files are saved to the %s disk.\n", fat_vol.label);
    } else if (boot_option == BOOT_OPT_DEMO) {
        console_set_color(YELLOW, BLACK);
        printf("[ ! ] You are in DEMO mode, no disks are loaded and files will not be written.\n");
    } else {
        console_set_color(RED, BLACK);
        printf("[ ! ] You are in VFS mode, your files will not be written.\n");
    }
    // wrong passwords typed for this account since its last login (shown once, after logging in)
    if (auth_failures_seen > 0) {
        console_set_color(YELLOW, BLACK);
        printf("[ ! ] %u wrong password(s) were typed for %s since your last login.\n", auth_failures_seen, auth_user);
        if (auth_failures_seen >= AUTH_MAX_FAILURES) {
            printf("[ ! ] Your home folder was wiped %u time(s) because of them.\n",
                   auth_failures_seen / AUTH_MAX_FAILURES);
        }
        auth_failures_seen = 0;
    }
    // the normal terminal colours (light grey on black) for everything after the banner
    console_set_color(BRIGHT, BLACK);
    printf("Type HELP for a list of commands, or DOCS to read the manual.\n\n");
    // forget any keys pressed while something else had the screen (e.g. the splash screen or a menu),
    // so they don't turn up at the start of the next command
    ClearInputbuffer();
    // make sure the keyboard driver is storing key presses again
    InputBufferChangeState(1);
    return 0;
}

// the prompt, the user's name, a fixed "@0/" and the current directory (cd always starts and ends with '/')
// e.g. "alex@0//: " at the root, "alex@0//sys/: " in /sys
void cli_prompt (void) {
    console_set_color(LIGHT_GREEN, BLACK);
    printf("%s", auth_user);
    console_set_color(BRIGHT, BLACK);
    printf("@0/%s: ", cd);
}

// every command takes the rest of the line (trimmed, "" when there is none)

// cli_commands[] (further down) needs cli_help's address and cli_help needs to walk cli_commands[],
// so declare it here and define it after the table
static void cli_help (char* args);

// password prompts, defined with the account commands further down
static void cli_ask_secret (const char* question, char* out, int size);

// encrypted files (see sys/fcrypt.h)

#define CLI_PASSWORD_MAX 64

// true (with a message) if path is an encrypted file, for commands that would break one
static bool cli_refuse_encrypted (const char* path) {
    if (!fcrypt_file_is_encrypted(path)) return false;
    printf("%s is encrypted, DECRYPT it first.\n", path);
    return true;
}

// RD on an encrypted file: ask for the password and show it, without changing the file
static void cli_read_encrypted (const char* path) {
    char password[CLI_PASSWORD_MAX];
    cli_ask_secret("Password: ", password, sizeof(password));
    uint8_t* contents;
    uint32_t length;
    ui_busy_begin("Checking the password");
    int result = fcrypt_read_file(path, password, &contents, &length);
    ui_busy_end();
    crypto_wipe(password, sizeof(password));
    if (result != FCRYPT_OK) {
        printf("Can't read %s: %s\n", path, fcrypt_strerror(result));
        return;
    }
    printf((length > 0 && contents[length - 1] == '\n') ? "%s" : "%s\n", (char*)contents);
    crypto_wipe(contents, length);
    free(contents);
}

// ENCRYPT <FILE>: lock a file with a password (asked for twice)
static void cli_encrypt (char* args) {
    if (*args == '\0') {
        printf("Usage: ENCRYPT <file>\n");
        return;
    }
    char password[CLI_PASSWORD_MAX], again[CLI_PASSWORD_MAX];
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    if (!fs_split_path(args, dir, sizeof(dir), name, sizeof(name)) || !fs_file_exists(dir, name)) {
        printf("File not found: %s\n", args);
        return;
    }
    if (fcrypt_file_is_encrypted(args)) {
        printf("%s is already encrypted.\n", args);
        return;
    }
    cli_ask_secret("Password: ", password, sizeof(password));
    cli_ask_secret("Again:    ", again, sizeof(again));
    if (strcmp(password, again) != 0) {
        printf("The passwords don't match, nothing was changed.\n");
    } else {
        ui_busy_begin("Encrypting");
        int result = fcrypt_encrypt_file(args, password);
        ui_busy_end();
        if (result == FCRYPT_OK) {
            printf("Encrypted %s. Don't lose the password, there is no way back without it.\n", args);
        } else {
            printf("Couldn't encrypt %s: %s\n", args, fcrypt_strerror(result));
        }
    }
    crypto_wipe(password, sizeof(password));
    crypto_wipe(again, sizeof(again));
}

// DECRYPT <FILE>: turn an encrypted file back into a normal one
static void cli_decrypt (char* args) {
    if (*args == '\0') {
        printf("Usage: DECRYPT <file>\n");
        return;
    }
    if (!fcrypt_file_is_encrypted(args)) {
        printf("%s isn't an encrypted file.\n", args);
        return;
    }
    char password[CLI_PASSWORD_MAX];
    cli_ask_secret("Password: ", password, sizeof(password));
    ui_busy_begin("Decrypting");
    int result = fcrypt_decrypt_file(args, password);
    ui_busy_end();
    crypto_wipe(password, sizeof(password));
    if (result == FCRYPT_OK) {
        printf("Decrypted %s.\n", args);
    } else {
        printf("Couldn't decrypt %s: %s\n", args, fcrypt_strerror(result));
    }
}

// LA: list the files and directories in the current directory
static void cli_list (char* args) {
    (void)args; // LA takes no arguments, anything after it is ignored
    fs_print();
}

// SD <dir>: set (change) the current directory
// the path can be absolute or relative and use "." and "..", fs_set_cd() sorts that out
static void cli_set_directory (char* args) {
    if (*args == '\0') {
        printf("Usage: SD <directory>\n");
        return;
    }
    if (fs_set_cd(args) != 0) {
        printf("Directory not found: %s\n", args);
    }
}

// CLK: the date and time from the CMOS real time clock
// (in an emulator this is usually the host's time, often in UTC)
static void cli_clock (char* args) {
    (void)args;
    rtc_time_t t;
    rtc_read(&t);
    // day-month-year, the British way round
    printf("System time: %02u-%02u-%04u %02u:%02u:%02u\n", t.day, t.month, t.year, t.hour, t.minute, t.second);
}

// TIME: how long the OS has been running, counted by the 1000Hz system timer
static void cli_uptime (char* args) {
    (void)args;
    uint32_t ms = clck_uptime_ms();
    uint32_t seconds = ms / 1000;
    // split the seconds into hours, minutes (0 - 59) and seconds (0 - 59)
    printf("Time since boot: %uh %02um %02us (%u ms)\n", seconds / 3600, (seconds / 60) % 60, seconds % 60, ms);
}

// PMAN: print the process stack (you will see the terminal itself there as RUNNING)
static void cli_pman (char* args) {
    (void)args;
    printf("> PMAN:\n");
    stack_print();
}

// RUN <code>: run a line of AerLang as a new process, e.g. "run s:hello" or "run print_stack"
// the process lives just for this command: create, execute, destroy
// unknown code shows the red error screen, the terminal is redrawn afterwards (see HandleInput)
static void cli_run (char* args) {
    if (*args == '\0') {
        printf("Usage: RUN <code>   (e.g. run print_stack)\n");
        return;
    }
    int pid = p_create(args);
    if (pid < 0) {
        printf("The process stack is full.\n");
        return;
    }
    // the terminal is RUNNING, so p_exec() runs the new process without starting the terminal again
    p_exec(pid);
    p_destroy(pid);
}

// RD <file>: print a file's contents
static void cli_read_file (char* args) {
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    // "notes.txt" -> cd + "notes.txt", "/sys/conf.aer" -> "/sys/" + "conf.aer"
    // fails for an empty or invalid name
    if (!fs_split_path(args, dir, sizeof(dir), name, sizeof(name))) {
        printf("Usage: RD <file>\n");
        return;
    }
    if (fcrypt_file_is_encrypted(args)) {
        cli_read_encrypted(args);
        return;
    }
    // fs_read_f() gives us our own copy of the contents, which we must free()
    char* contents = fs_read_f(dir, name);
    if (contents == NULL) {
        printf("File not found: %s\n", args);
        return;
    }
    size_t length = strlen(contents);
    // only add a newline if the file doesn't already end with one, so the prompt always starts on a fresh line
    // (both format strings are fixed text, so picking one at run time is safe)
    printf((length > 0 && contents[length - 1] == '\n') ? "%s" : "%s\n", contents);
    free(contents);
}

// MKF <file> [text]: make a new file, optionally with some text already in it
static void cli_make_file (char* args) {
    // split off the file name at the first space, everything after it (spaces and all) is the text
    // parts[1] stays NULL if there was no space
    char* parts[2] = { args, NULL };
    int n = split(args, ' ', parts, 2);
    const char* text = (n > 1) ? StrTrim(parts[1]) : "";
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];

    if (!fs_split_path(parts[0], dir, sizeof(dir), name, sizeof(name))) {
        printf("Usage: MKF <file> [text]\n");
        return;
    }
    // fails if the file already exists, the directory doesn't, or the disk / VFS is full
    int result = fs_create_f(dir, name, text);
    if (result != FS_OK) {
        printf("Could not create %s: %s\n", parts[0], fs_strerror(result));
    }
}

// WR <file> <text>: add a line of text to the end of an existing file
static void cli_write_file (char* args) {
    // file name, then the text (which can contain spaces)
    char* parts[2] = { args, NULL };
    int n = split(args, ' ', parts, 2);
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];

    // both the name and the text are needed here, unlike MKF
    if (n < 2 || !fs_split_path(parts[0], dir, sizeof(dir), name, sizeof(name))) {
        printf("Usage: WR <file> <text>\n");
        return;
    }
    if (cli_refuse_encrypted(parts[0])) return; // adding text would break the file's tag
    // read the file first, to check it exists and to see whether it is empty
    char* existing = fs_read_f(dir, name);
    if (existing == NULL) {
        printf("File not found: %s\n", parts[0]);
        return;
    }
    bool empty = existing[0] == '\0';
    free(existing);

    // add the text as a new line
    // a non empty file gets "\n<text>", an empty one just "<text>", so the file never starts with a blank line
    char* text = StrTrim(parts[1]);
    // + 2 = room for the '\n' and the terminating '\0'
    char* line = malloc(strlen(text) + 2);
    if (line == NULL) {
        printf("Out of memory.\n");
        return;
    }
    strcpy(line, empty ? "" : "\n");
    strcat(line, text);

    // true = append to the end rather than replace the contents
    int result = fs_write_f(dir, name, line, true);
    free(line);
    if (result != FS_OK) {
        printf("Could not write %s: %s\n", parts[0], fs_strerror(result));
    }
}

// MKD <dir>: make a new directory
static void cli_make_directory (char* args) {
    char path[FS_PATH_MAX];
    char parent[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    // fs_resolve_dir() turns e.g. "cache" (in /sys/) into "/sys/cache/"
    // "/" is refused here, it always exists and has no name (the name code below would also go wrong on it)
    if (*args == '\0' || !fs_resolve_dir(args, path, sizeof(path)) || StrCompare(path, "/")) {
        printf("Usage: MKD <directory>\n");
        return;
    }
    // the last part of the path is the directory's name
    // e.g. path "/sys/cache/" -> parent "/sys/" -> name "cache/" -> "cache"
    fs_parent_path(path, parent, sizeof(parent));
    strlcpy(name, path + strlen(parent), sizeof(name));
    name[strlen(name) - 1] = '\0'; // drop the trailing '/'
    int result = fs_create_d(path, name);
    if (result != FS_OK) {
        printf("Could not create %s: %s\n", path, fs_strerror(result));
    }
}

// DEL <name>: delete a file, or a directory if it is empty
// the same name could be read as a file or a directory, so a file is tried first
// moves DEL's progress bar along, one deleted file at a time
typedef struct {
    ui_progress_t bar;
    uint32_t done;
} cli_delete_progress_t;

static void cli_delete_step (const char* name, void* ctx) {
    cli_delete_progress_t* d = ctx;
    ui_progress_update(&d->bar, ++d->done, name);
}

// a listing callback that does nothing, for when only fs_list's count is wanted
static void cli_ignore_entry (const char* name, bool is_dir, uint32_t size, void* ctx) {
    (void)name; (void)is_dir; (void)size; (void)ctx;
}

// DEL on a directory: delete it, and everything in it once the user says yes
static void cli_delete_dir (const char* path) {
    // the same rules fs_delete_d has, checked BEFORE emptying anything: never the root, and
    // never the directory we are standing in (or one it is inside)
    if (StrCompare(path, "/")) {
        printf("The top folder, /, can't be deleted.\n");
        return;
    }
    if (strncmp(cd, path, strlen(path)) == 0) {
        printf("Can't delete %s: you are in it (SD somewhere else first).\n", path);
        return;
    }
    int files = fs_count_files(path);
    int inside = fs_list(path, cli_ignore_entry, NULL); // fs_list returns how many entries there are
    if (inside > 0) {
        printf("%s has %d file(s) in it. Delete it and everything in it? (y/n) ", path, files < 0 ? 0 : files);
        int key = GetChar();
        printf("%c\n", key == 'y' || key == 'Y' ? 'y' : 'n');
        if (key != 'y' && key != 'Y') {
            printf("Nothing was deleted.\n");
            return;
        }
        cli_delete_progress_t d = { { 0, 0, 0 }, 0 };
        ui_progress_begin(&d.bar, files > 0 ? (uint32_t)files : 1);
        int deleted = fs_empty_dir(path, false, cli_delete_step, &d);
        ui_progress_end(&d.bar);
        printf("Deleted %d file(s).\n", deleted < 0 ? 0 : deleted);
    }
    int result = fs_delete_d(path);
    if (result != FS_OK) {
        printf("Can't delete %s: %s\n", path, fs_strerror(result));
    }
}

static void cli_delete (char* args) {
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    char path[FS_PATH_MAX];

    if (*args == '\0') {
        printf("Usage: DEL <file or directory>\n");
        return;
    }
    // a file?
    if (fs_split_path(args, dir, sizeof(dir), name, sizeof(name)) && fs_file_exists(dir, name)) {
        int result = fs_delete_f(dir, name);
        if (result != FS_OK) {
            printf("Can't delete %s: %s\n", args, fs_strerror(result));
        }
        return;
    }
    // a directory? An empty one just goes, one with things in it is emptied first (after asking)
    if (fs_resolve_dir(args, path, sizeof(path)) && fs_dir_exists(path)) {
        cli_delete_dir(path);
        return;
    }
    // neither
    printf("Not found: %s\n", args);
}

// ECHO <text>: print the text back
static void cli_echo (char* args) {
    printf("%s\n", args);
}

// CLS: clear the screen
// this uses console_clear(), not clear(), so vga_clear_count stays the same and HandleInput does
// not redraw the banner: you get a genuinely empty screen with just the prompt
static void cli_clear (char* args) {
    (void)args;
    console_clear();
}

// MEM: memory usage, the machine's memory (from GRUB), the kernel heap, the process stack and storage
static void cli_memory (char* args) {
    (void)args;
    // the machine's memory is only known if GRUB told us about it (see sys/boot.c)
    if (boot_multiboot_valid) {
        printf("System memory: %u KB lower, %u KB upper (%u MB total)\n",
               sys_mem_lower_kb, sys_mem_upper_kb, (sys_mem_lower_kb + sys_mem_upper_kb) / 1024);
    }
    printf("Kernel heap: %u / %u bytes used\n", (unsigned int)heap_used(), (unsigned int)heap_total());
    printf("Processes: %d / %d\n", p_cnt, P_STACK_SIZE);
    if (fs_mode == FS_MODE_DISK) {
        // 2 sectors = 1 KB (work in sectors, clusters can be smaller than 1 KB)
        // a sector is 512 bytes, so a cluster of 1 sector is only half a KB and "cluster size in KB"
        // would round down to 0. Multiplying out the sectors first keeps it exact
        uint32_t free_kb = fat_free_clusters() * fat_vol.sectors_per_cluster / 2;
        uint32_t total_kb = fat_vol.cluster_count * fat_vol.sectors_per_cluster / 2;
        printf("Disk: FAT%d \"%s\", %u KB free of %u KB\n", fat_vol.type, fat_vol.label, free_kb, total_kb);
    } else {
        // in VFS mode the limits are the fixed size file and directory tables
        printf("VFS: %d / %d files, %d / %d directories\n", f_c, FS_MAX_FILES, d_c, FS_MAX_DIRS);
    }
}

// DRV: list what is plugged into each of the 4 IDE positions (2 buses, each with a master and a slave)
static void cli_drives (char* args) {
    (void)args;
    static const char* const bus_names[2] = { "Primary", "Secondary" };
    for (int bus = 0; bus < 2; bus++) {
        for (int slave = 0; slave < 2; slave++) {
            // the ATA IDENTIFY model name is up to 40 characters, plus the '\0'
            char model[41];
            uint32_t sectors;
            // returns the kind of drive (DRIVE_NONE, DRIVE_ATA ...) and fills in its model and size
            int type = drive_probe_info(bus, slave, model, sizeof(model), &sectors);
            printf("%-9s %-6s : %s %s", bus_names[bus], slave ? "slave" : "master", drive_type_name(type), model);
            // 2048 sectors of 512 bytes = 1 MB (only ATA hard drives report a size, CD drives and empty positions give 0)
            if (sectors) {
                printf(" (%u MB)", sectors / 2048);
            }
            // point out the drive the file system is using
            if (fs_mode == FS_MODE_DISK && fat_vol.bus == bus && fat_vol.slave == slave) {
                printf(" [mounted]");
            }
            printf("\n");
        }
    }
}

// BEEP [hz]: play a beep on the PC speaker
// atoi() gives 0 for no argument (or something that isn't a number), and beep(0) is the default beep
static void cli_beep (char* args) {
    beep(atoi(args));
}

// PLAY's status line. While the file is read and decoded a "Reading" spinner turns (busy(),
// called by the MP3 decoder). The sound card asks cli_play_should_stop every few milliseconds
// once it is playing, which is when the line becomes "Playing song.mp3  0:12  Esc stops it"
static struct {
    const char* name;      // the file
    bool playing;          // the card has started
    uint8_t row;           // the status line's screen row
    uint32_t started_ms;   // when the card started
    uint32_t drawn_ms;     // when the line was last redrawn
} cli_playing;

static bool cli_play_should_stop (void) {
    uint32_t now = clck_uptime_ms();
    if (!cli_playing.playing) {
        ui_busy_end(); // the reading spinner's line becomes the playing line
        uint8_t x;
        console_get_cursor(&x, &cli_playing.row);
        printf("\n");
        cli_playing.playing = true;
        cli_playing.started_ms = now;
        cli_playing.drawn_ms = 0;
    }
    if (cli_playing.drawn_ms == 0 || now - cli_playing.drawn_ms >= 100) {
        cli_playing.drawn_ms = now;
        uint32_t seconds = (now - cli_playing.started_ms) / 1000;
        char line[COLS + 1];
        snprintf(line, sizeof(line), "Playing %s  %u:%02u  (Esc stops it)", cli_playing.name,
                 seconds / 60, seconds % 60);
        int width = ui_spinner(2, cli_playing.row, 4, LIGHT_GREEN);
        puts((uint8_t)(3 + width), cli_playing.row, BRIGHT, console_bg, line);
    }
    return kb_take_break() || (kb_haschar() && GetChar() == KEY_ESCAPE);
}

// PLAY <file>: play a WAV or MP3 file on the sound card
static void cli_play (char* args) {
    if (*args == '\0') {
        printf("Usage: PLAY <file>   (a .wav or .mp3 file)\n");
        return;
    }
    cli_playing.name = args;
    cli_playing.playing = false;
    char label[COLS];
    snprintf(label, sizeof(label), "Reading %s", args);
    ui_busy_begin(label);
    int result = sound_play_file(args, cli_play_should_stop);
    if (!cli_playing.playing) {
        ui_busy_end(); // it never started (a missing file, or not a sound file)
    }
    if (result != SOUND_OK) {
        printf("Can't play %s: %s\n", args, sound_strerror(result));
    }
}

// REBOOT: restart the computer, never returns
static void cli_reboot (char* args) {
    (void)args;
    sys_reboot();
}

// SHUTDOWN: turn the computer off (or show "It is now safe to turn off your computer"), never returns
static void cli_shutdown (char* args) {
    (void)args;
    sys_shutdown();
}

// the interactive AerLang prompt
static void cli_aer (char* args) {
    (void)args;
    aer_repl();
}

// save the resolution for the next start, as a GRUB environment block in the AEROS disk's root:
// exactly 1024 bytes, a fixed first line, "name=value" lines and '#' padding. grub.cfg reads it
// with load_env and asks for that mode before starting AerOS (which works on any PC)
static bool save_resolution (uint32_t width, uint32_t height) {
    if (fs_mode != FS_MODE_DISK) {
        return false;
    }
    char block[1025];
    int used = snprintf(block, sizeof(block), "# GRUB Environment Block\naeros_mode=%ux%ux32\n", width, height);
    memset(block + used, '#', (size_t)(1024 - used));
    block[1024] = '\0';
    int result = fs_file_exists("/", "grubenv") ? fs_write_f("/", "grubenv", block, false) : fs_create_f("/", "grubenv", block);
    return result >= 0;
}

// RES: show the screen resolution, or change it with RES <width>x<height>
static void cli_resolution (char* args) {
    vga_framebuffer_t fb;
    bool graphics = vga_framebuffer_info(&fb);
    if (*args == '\0') {
        if (graphics) {
            printf("Screen: %ux%u, %u bits per pixel\n", fb.width, fb.height, (unsigned int)fb.bpp);
        } else {
            printf("Screen: VGA text mode\n");
        }
        printf("Change it with RES <width>x<height>, e.g. RES 1280x720\n");
        printf("Common sizes: 640x480 800x600 1024x768 1280x720 1280x1024 1600x900 1920x1080\n");
        return;
    }

    char* end;
    long width = strtol(args, &end, 10);
    long height = (*end == 'x' || *end == 'X') ? strtol(end + 1, &end, 10) : 0;
    if (*end != '\0' || width < 640 || height < 400 || width > 4096 || height > 4096) {
        printf("Usage: RES <width>x<height>, at least 640x400 (e.g. RES 1280x720)\n");
        return;
    }

    bool changed = bga_set_mode((uint32_t)width, (uint32_t)height);
    bool saved = save_resolution((uint32_t)width, (uint32_t)height);
    if (changed) {
        vga_framebuffer_info(&fb);
        printf("The screen is now %ux%u.\n", fb.width, fb.height);
    }
    if (saved) {
        printf("Saved: AerOS will start at %ldx%ld from now on.\n", width, height);
    } else if (changed) {
        printf("Not saved for the next start, that needs an AEROS disk.\n");
    } else {
        printf("This graphics card can't change resolution while AerOS is running,\n");
        printf("and without an AEROS disk it can't be saved for the next start either.\n");
    }
}

static void cli_edit (char* args) {
    if (*args == '\0') {
        printf("Usage: EDIT <file>   (e.g. edit hello.aer)\n");
        return;
    }
    if (cli_refuse_encrypted(args)) return;
    editor_open(args);
}

// DOCS [topic]: the manual in the reader app, "docs sound" opens /docs/sound.md
static void cli_docs (char* args) {
    char path[FS_PATH_MAX];
    if (*args == '\0') {
        strlcpy(path, "/docs/index.md", sizeof(path));
    } else {
        snprintf(path, sizeof(path), "/docs/%s%s", args, strchr(args, '.') ? "" : ".md");
    }
    aer_run_file("/bin/read.aer", path);
}

// is there a program file with this name, here or in /bin/?
static bool program_exists (const char* file) {
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (!fs_split_path(file, dir, sizeof(dir), name, sizeof(name))) {
        return false;
    }
    return fs_file_exists(dir, name) || (!strchr(file, '/') && fs_file_exists("/bin/", name));
}

// turn "hello" + "a b" into the process code "hello.aer a b", false if there is no such program
static bool cli_program_line (const char* command, const char* args, char* line, size_t size) {
    strlcpy(line, command, size);
    size_t length = strlen(line);
    if (!aer_is_program_name(line, length)) {
        strlcat(line, ".aer", size);
    }
    if (!program_exists(line)) {
        return false;
    }
    if (*args) {
        strlcat(line, " ", size);
        strlcat(line, args, size);
    }
    return true;
}

// run "command args" as an AerLang program if command names one ("hello" or "hello.aer"),
// returns false if there is no such program
static bool cli_run_program (const char* command, const char* args) {
    char line[P_INST_MAX];
    if (!cli_program_line(command, args, line, sizeof(line))) {
        return false;
    }
    // like RUN: as its own process, which hands the line to compile()
    int pid = p_create(line);
    if (pid < 0) {
        printf("The process stack is full.\n");
        return true;
    }
    p_exec(pid);
    p_destroy(pid);
    return true;
}

// ---- the queue (see "The queue" in sys/p_man.h) ---------------------------------------------------

static void cli_queue_print (int position, const char* code, void* ctx) {
    (void)ctx;
    printf("  %d. %s\n", position, code);
}

// QUEUE <program> [args] / QUEUE / QUEUE CLEAR
static void cli_queue (char* args) {
    if (*args == '\0') {
        if (p_queue_count() == 0) {
            printf("The queue is empty. QUEUE <program> adds one, GO runs them in order.\n");
        } else {
            printf("Queued, in the order GO will run them:\n");
            p_queue_each(cli_queue_print, NULL);
        }
        return;
    }
    if (strcasecmp(args, "clear") == 0) {
        printf("Took %d program(s) out of the queue.\n", p_queue_clear());
        return;
    }
    // a program and its arguments, or "s:<text>" to print a line between programs
    char line[P_INST_MAX];
    if (strncmp(args, "s:", 2) == 0) {
        strlcpy(line, args, sizeof(line));
    } else {
        char* parts[2] = { args, NULL };
        char none[1] = "";
        int n = split(args, ' ', parts, 2);
        if (!cli_program_line(parts[0], n > 1 ? StrTrim(parts[1]) : none, line, sizeof(line))) {
            printf("There is no program called %s to queue.\n", parts[0]);
            return;
        }
    }
    int count = p_queue_add(line);
    if (count < 0) {
        printf("The process stack is full.\n");
    } else {
        printf("Queued %s (%d in the queue, GO runs them).\n", line, count);
    }
}

// GO: before each queued program, a line saying which one it is
static void cli_go_next (int number, int total, const char* code) {
    console_set_color(DARK_GRAY, BLACK);
    printf("[%d/%d] %s\n", number, total, code);
    console_set_color(BRIGHT, BLACK);
}

// GO: Ctrl+C in a queued program stops the rest of the queue too
static bool cli_go_stop (void) {
    return aer_take_stopped();
}

// GO: run everything in the queue, oldest first
static void cli_go (char* args) {
    (void)args;
    if (p_queue_count() == 0) {
        printf("The queue is empty. QUEUE <program> adds one.\n");
        return;
    }
    aer_take_stopped(); // forget a stop from before
    int ran = p_queue_run(cli_go_next, cli_go_stop);
    int left = p_queue_count();
    if (left > 0) {
        printf("Stopped the queue after %d program(s), %d still queued (GO carries on).\n", ran, left);
    } else {
        printf("Ran %d program(s), the queue is empty.\n", ran);
    }
}

// accounts (see sys/accounts/auth.h)

// the account commands only make sense with a disk to keep the accounts on
static bool cli_accounts_available (void) {
    if (!auth_required()) {
        printf("Accounts need an AEROS disk, without one everyone is guest.\n");
        return false;
    }
    return true;
}

// ask for a password without showing it
static void cli_ask_secret (const char* question, char* out, int size) {
    printf("%s", question);
    GetSecret(out, size);
}

// ask for a new password twice, false (with a message) if it was empty or the two didn't match
static bool cli_ask_new_password (char* out, int size) {
    char again[CLI_PASSWORD_MAX];
    cli_ask_secret("New password: ", out, size);
    cli_ask_secret("Again:        ", again, sizeof(again));
    bool ok = out[0] != '\0' && strcmp(out, again) == 0;
    if (!ok) {
        printf("The passwords %s, nothing was changed.\n", out[0] ? "don't match" : "can't be empty");
    }
    crypto_wipe(again, sizeof(again));
    return ok;
}

// ask for the logged in user's own password before doing something drastic
static bool cli_confirm_password (void) {
    char password[CLI_PASSWORD_MAX];
    cli_ask_secret("Your password: ", password, sizeof(password));
    ui_busy_begin("Checking the password");
    bool ok = auth_check(auth_user, password);
    ui_busy_end();
    crypto_wipe(password, sizeof(password));
    if (!ok) {
        printf("Wrong password.\n");
    }
    return ok;
}

// WHOAMI: print who is logged in
static void cli_whoami (char* args) {
    (void)args;
    printf("%s\n", auth_user);
}

static void cli_print_user (const char* name, void* ctx) {
    (void)ctx;
    printf("%s%s\n", name, StrCompare(name, auth_user) ? "  (you)" : "");
}

// USERS: list every account
static void cli_users (char* args) {
    (void)args;
    if (cli_accounts_available()) {
        auth_list(cli_print_user, 0);
    }
}

// ADDUSER <NAME>: make a new account
static void cli_add_user (char* args) {
    if (!cli_accounts_available()) return;
    strlwr(args);
    if (*args == '\0') {
        printf("Usage: ADDUSER <NAME>\n");
        return;
    }
    // check the name before asking for a password, not after
    if (!auth_valid_new_name(args)) {
        printf("Can't use that name: %s\n", auth_strerror(AUTH_ERR_NAME));
        return;
    }
    if (auth_exists(args)) {
        printf("%s already exists.\n", args);
        return;
    }
    char password[CLI_PASSWORD_MAX];
    if (cli_ask_new_password(password, sizeof(password))) {
        ui_busy_begin("Saving the account");
        int result = auth_add(args, password);
        ui_busy_end();
        if (result == AUTH_OK) {
            char home[FS_PATH_MAX] = "";
            printf("Made the account %s", args);
            printf(auth_home(args, home, sizeof(home)) ? ", with the home folder %s.\n" : ".\n", home);
        } else {
            printf("Couldn't make the account: %s\n", auth_strerror(result));
        }
    }
    crypto_wipe(password, sizeof(password));
}

// DELUSER <NAME>: delete someone else's account (your own password is asked for first)
static void cli_delete_user (char* args) {
    if (!cli_accounts_available()) return;
    strlwr(args);
    if (*args == '\0') {
        printf("Usage: DELUSER <NAME>\n");
        return;
    }
    if (StrCompare(args, auth_user)) {
        printf("You can't delete the account you are logged in as.\n");
        return;
    }
    if (!auth_exists(args)) {
        printf("There is no user called %s.\n", args);
        return;
    }
    if (!cli_confirm_password()) return;
    int result = auth_delete(args);
    printf(result == AUTH_OK ? "Deleted %s.\n" : "Couldn't delete %s.\n", args);
}

// PASSWD: change your own password
static void cli_password (char* args) {
    (void)args;
    if (!cli_accounts_available() || !cli_confirm_password()) return;
    char password[CLI_PASSWORD_MAX];
    if (cli_ask_new_password(password, sizeof(password))) {
        ui_busy_begin("Saving the password");
        int result = auth_set_password(auth_user, password);
        ui_busy_end();
        printf(result == AUTH_OK ? "Password changed.\n" : "Couldn't save the new password.\n");
    }
    crypto_wipe(password, sizeof(password));
}

// LOGOUT: back to the login screen
static void cli_logout (char* args) {
    (void)args;
    if (!cli_accounts_available()) return;
    ClearInputHistory(); // the next person shouldn't be able to scroll back through these commands
    strlcpy(cd, "/", sizeof(cd)); // the next user starts at the root
    cli_logged_in = false;
}

// the commands, in the order HELP lists them (names are not case sensitive)
// name = what the user types (lowercase here, matched with strcasecmp so LA, la and La all work)
// run  = the handler, given the trimmed rest of the line
// help = the line HELP prints for it
static const struct cli_command {
    const char* name;
    void (*run)(char* args);
    const char* help;
} cli_commands[] = {
    { "help",     cli_help,           "HELP            - THIS COMMAND" },
    { "la",       cli_list,           "LA              - LIST ALL FILES AND DIRECTORIES IN THE CURRENT DIRECTORY" },
    { "sd",       cli_set_directory,  "SD <DIR>        - SET THE CURRENT DIRECTORY (e.g. SD /sys, SD .., SD /)" },
    { "clk",      cli_clock,          "CLK             - PRINTS THE SYSTEM TIME" },
    { "time",     cli_uptime,         "TIME            - PRINTS THE TIME SINCE BOOT" },
    { "pman",     cli_pman,           "PMAN            - PRINTS THE PROCESS STACK" },
    { "queue",    cli_queue,          "QUEUE [PROGRAM] - QUEUE A PROGRAM FOR GO, OR LIST THE QUEUE (QUEUE CLEAR)" },
    { "go",       cli_go,             "GO              - RUN EVERYTHING IN THE QUEUE, IN ORDER (CTRL+C STOPS IT)" },
    { "run",      cli_run,            "RUN <CODE>      - RUN CODE AS A PROCESS (e.g. run print_stack, run s:hi)" },
    { "rd",       cli_read_file,      "RD <FILE>       - READ A FILE" },
    { "mkf",      cli_make_file,      "MKF <FILE> [T]  - MAKE A FILE, WITH OPTIONAL TEXT" },
    { "wr",       cli_write_file,     "WR <FILE> <T>   - ADD A LINE OF TEXT TO THE END OF A FILE" },
    { "mkd",      cli_make_directory, "MKD <DIR>       - MAKE A DIRECTORY" },
    { "del",      cli_delete,         "DEL <NAME>      - DELETE A FILE, OR A DIRECTORY AND ALL IN IT (ASKS FIRST)" },
    { "echo",     cli_echo,           "ECHO <TEXT>     - PRINT TEXT" },
    { "cls",      cli_clear,          "CLS             - CLEAR THE SCREEN" },
    { "mem",      cli_memory,         "MEM             - MEMORY USAGE" },
    { "drv",      cli_drives,         "DRV             - LIST IDE / ATA DRIVES" },
    { "beep",     cli_beep,           "BEEP [HZ]       - PLAY A BEEP ON THE PC SPEAKER" },
    { "play",     cli_play,           "PLAY <FILE>     - PLAY A .WAV OR .MP3 FILE (ESC TO STOP)" },
    { "reboot",   cli_reboot,        "REBOOT          - RESTART THE COMPUTER" },
    { "shutdown", cli_shutdown,       "SHUTDOWN        - TURN OFF THE COMPUTER" },
    { "res",      cli_resolution,     "RES [WxH]       - SHOW OR CHANGE THE SCREEN RESOLUTION (e.g. RES 1280x720)" },
    { "edit",     cli_edit,          "EDIT <FILE>     - EDIT A FILE (CTRL+S SAVE, CTRL+R RUN A .AER, CTRL+Q QUIT)" },
    { "aer",      cli_aer,            "AER             - THE AERLANG PROMPT, TYPE CODE TO RUN IT" },
    { "docs",     cli_docs,           "DOCS [TOPIC]    - READ THE MANUAL (e.g. DOCS SOUND), READ <FILE> FOR ANY FILE" },
    { "encrypt",  cli_encrypt,        "ENCRYPT <FILE>  - LOCK A FILE WITH A PASSWORD (RD ASKS FOR IT TO SHOW THE FILE)" },
    { "decrypt",  cli_decrypt,        "DECRYPT <FILE>  - UNLOCK AN ENCRYPTED FILE" },
    { "whoami",   cli_whoami,        "WHOAMI          - PRINT WHO IS LOGGED IN" },
    { "users",    cli_users,          "USERS           - LIST THE USER ACCOUNTS" },
    { "adduser",  cli_add_user,       "ADDUSER <NAME>  - MAKE A NEW USER ACCOUNT" },
    { "deluser",  cli_delete_user,    "DELUSER <NAME>  - DELETE A USER ACCOUNT" },
    { "passwd",   cli_password,       "PASSWD          - CHANGE YOUR PASSWORD" },
    { "logout",   cli_logout,         "LOGOUT          - LOG OUT, BACK TO THE LOGIN SCREEN" },
    { "",         0,                  "<APP> [ARGS]    - RUN AN AERLANG PROGRAM, e.g. HELLO (/bin/hello.aer)" },
};

// how many commands are in the table (the size of the whole array divided by the size of one entry)
#define CLI_COMMAND_COUNT (sizeof(cli_commands) / sizeof(cli_commands[0]))

// HELP: print the help line of every command, in table order
static void cli_help (char* args) {
    (void)args;
    printf("> HELP:\n");
    for (size_t i = 0; i < CLI_COMMAND_COUNT; i++) {
        printf("%s\n", cli_commands[i].help);
    }
}

// ---- Tab completion -----------------------------------------------------------------------------
//
// Like a Linux shell: press Tab part way through a word and the terminal finishes it.
//
//     the first word     commands (HELP, PLAY...), apps in /bin/ and .aer programs here
//     any later word     files and folders, relative ("no" -> "notes.txt") or with a path
//                        ("/bin/he" -> "/bin/hello.aer", "../" ...). After SD, only folders
//
//     one match          it is filled in, then a space (or "/" after a folder, to keep going)
//     several matches    filled in as far as they all agree ("re" -> "re" + "ad" for read / reader)
//     nothing to add     the matches are listed, then the prompt and the line come back
//
// Matching ignores upper / lower case, like the commands and the FAT file system do.

#define COMPLETE_MAX 128 // most matches gathered for one Tab

static struct {
    const char* prefix;        // the part of the word typed so far
    size_t prefix_length;
    bool folders_only;         // completing the argument of SD
    char names[COMPLETE_MAX][FS_NAME_MAX + 1]; // the matches, folders ending in '/'
    int count;
} completion;

// add a match unless it is already there (an app in /bin/ and one here can share a name)
static void complete_add (const char* name, size_t length) {
    if (completion.count == COMPLETE_MAX || length > FS_NAME_MAX) return;
    if (strncasecmp(name, completion.prefix, completion.prefix_length) != 0) return;
    for (int i = 0; i < completion.count; i++) {
        if (strlen(completion.names[i]) == length && strncasecmp(completion.names[i], name, length) == 0) return;
    }
    memcpy(completion.names[completion.count], name, length);
    completion.names[completion.count][length] = '\0';
    completion.count++;
}

// a directory entry while completing a path: folders get a '/' on the end
static void complete_entry (const char* name, bool is_dir, uint32_t size, void* ctx) {
    (void)size; (void)ctx;
    if (completion.folders_only && !is_dir) return;
    char full[FS_NAME_MAX + 2];
    snprintf(full, sizeof(full), "%s%s", name, is_dir ? "/" : "");
    complete_add(full, strlen(full));
}

// a directory entry while completing a command: only programs, without their ".aer"
static void complete_program (const char* name, bool is_dir, uint32_t size, void* ctx) {
    (void)size; (void)ctx;
    size_t length = strlen(name);
    if (!is_dir && aer_is_program_name(name, length)) complete_add(name, length - 4);
}

// print the matches in columns under the line, then the prompt and the line again
static void complete_list (const char* line) {
    size_t widest = 0;
    for (int i = 0; i < completion.count; i++) {
        size_t w = strlen(completion.names[i]);
        if (w > widest) widest = w;
    }
    int column = (int)widest + 2;
    int per_row = COLS / column > 0 ? COLS / column : 1;
    printf("\n");
    for (int i = 0; i < completion.count; i++) {
        bool last_in_row = (i + 1) % per_row == 0 || i + 1 == completion.count;
        if (last_in_row) {
            printf("%s\n", completion.names[i]);
        } else {
            printf("%-*s", column, completion.names[i]); // padded to the column width
        }
    }
    cli_prompt();
    printf("%s", line);
}

static int cli_complete (char* line, int length, int size) {
    // the word being completed starts after the last space
    int start = length;
    while (start > 0 && line[start - 1] != ' ') start--;
    const char* word = line + start;
    bool first_word = true;
    for (int i = 0; i < start; i++) {
        if (line[i] != ' ') first_word = false;
    }

    completion.count = 0;
    completion.folders_only = false;
    char dir[FS_PATH_MAX];
    if (first_word) {
        completion.prefix = word;
        completion.prefix_length = strlen(word);
        for (size_t i = 0; i < CLI_COMMAND_COUNT; i++) {
            if (cli_commands[i].name[0]) complete_add(cli_commands[i].name, strlen(cli_commands[i].name));
        }
        fs_list("/bin/", complete_program, NULL);
        fs_list(cd, complete_program, NULL);
    } else {
        // "docs/no" -> look in "docs/" for names starting "no"; "no" -> look here
        const char* slash = strrchr(word, '/');
        completion.prefix = slash ? slash + 1 : word;
        completion.prefix_length = strlen(completion.prefix);
        if (slash) {
            char typed_dir[FS_PATH_MAX];
            size_t dir_length = (size_t)(slash - word) + 1;
            if (dir_length >= sizeof(typed_dir)) return length;
            memcpy(typed_dir, word, dir_length);
            typed_dir[dir_length] = '\0';
            if (!fs_resolve_dir(typed_dir, dir, sizeof(dir))) return length;
        } else {
            strlcpy(dir, cd, sizeof(dir));
        }
        // the first word, to see if this is SD's argument
        char command[8] = "";
        int c = 0;
        while (line[c] == ' ') c++;
        int k = 0;
        while (line[c] && line[c] != ' ' && k < 7) command[k++] = line[c++];
        command[k] = '\0';
        completion.folders_only = strcasecmp(command, "sd") == 0;
        fs_list(dir, complete_entry, NULL);
    }
    if (completion.count == 0) return length; // nothing starts like that

    // how far all the matches agree, past what has been typed
    size_t common = strlen(completion.names[0]);
    for (int i = 1; i < completion.count; i++) {
        size_t j = completion.prefix_length;
        while (j < common && completion.names[i][j] &&
               tolower((unsigned char)completion.names[i][j]) == tolower((unsigned char)completion.names[0][j])) {
            j++;
        }
        common = j;
    }

    if (common > completion.prefix_length || completion.count == 1) {
        // add the agreed part. A single match is a finished name, so a space follows it, except
        // after a folder, where the next Tab carries on inside it
        const char* match = completion.names[0];
        const char* add = match + completion.prefix_length;
        size_t add_length = common - completion.prefix_length;
        bool finished = completion.count == 1 && match[strlen(match) - 1] != '/';
        for (size_t i = 0; i < add_length && length < size - 1; i++) {
            line[length++] = add[i];
            console_putchar(add[i]);
        }
        if (finished && length < size - 1) {
            line[length++] = ' ';
            console_putchar(' ');
        }
        line[length] = '\0';
        return length;
    }
    // several matches and nothing more they agree on: show them
    complete_list(line);
    return length;
}

// read the command and run it
// e.g. "  mkf notes.txt hello there " -> command "mkf", args "notes.txt hello there"
void HandleInput (char* in) {
    // drop spaces from both ends, so "  la  " still works
    char* line = StrTrim(in);
    if (*line == '\0') {
        return; // buffer is empty
    }

    // split the line into the command and its arguments
    // split() cuts the line in place at the first space only, so the arguments keep their own spaces
    // no_args is a real (writable) empty string for commands with no arguments, because the handlers
    // take a char* and must never be handed a string literal they might write to
    char no_args[1] = "";
    char* parts[2] = { line, no_args };
    int n = split(line, ' ', parts, 2);
    char* command = parts[0];
    // a line like "sd    /sys" leaves extra spaces at the front of the arguments, trim those too
    char* args = (n > 1) ? StrTrim(parts[1]) : no_args;

    // remember how many full screen clears there have been, to compare once the command is done
    unsigned int clears_before = vga_clear_count;

    // find the command in the table (not case sensitive)
    size_t i = 0;
    while (i < CLI_COMMAND_COUNT && strcasecmp(command, cli_commands[i].name) != 0) {
        i++;
    }
    // i is only inside the table if the loop stopped on a match
    if (i < CLI_COMMAND_COUNT) {
        cli_commands[i].run(args);
    } else if (cli_run_program(command, args)) {
        // it was an AerLang program (e.g. "hello" runs /bin/hello.aer)
    } else {
        printf("> %s\n", command);
        printf("Unrecognised command, please try again.\n");
    }

    // a program drew a full screen (e.g. the splash or error screen), bring the terminal back
    // (only clear() counts up vga_clear_count, the console's own clearing such as CLS does not)
    if (vga_clear_count != clears_before) {
        cli_rend_main();
    }
}

// terminal interface

/*
    t_int.c - the AerOS terminal

    This is where the user spends their time once the boot has finished. kernel.c creates a
    process called "terminal", the compiler turns that into cli_init(0), and from then on
    the terminal loop below is the OS:

        cli_rend_main()            draw the banner once
        while the terminal is up:
            cli_prompt()           e.g. "0//sys/: "
            GetString()            read a line (the keyboard driver echoes it and keeps history)
            HandleInput()          find the command in cli_commands[] and run it

    Because the terminal is itself a RUNNING process, the RUN command (which runs a line of
    RenLang as a new process) works by nesting: the new process is run from inside the
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
#include "../../lang/renlang.h"
#include "editor.h"
#include "../../drivers/video/bga.h"
#include "../../lib/crypto.h"
#include "../accounts/auth.h"

// the longest line the user can type, including the terminating '\0'
#define CLI_INPUT_MAX 128

bool cli_active = false; // true from cli_init() onwards, compile() checks it before starting another terminal
static char cli_input[CLI_INPUT_MAX]; // the line being typed, re-used for every command

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
            // gotcha: with no keyboard it returns an empty line straight away, so the prompt just repeats
            GetString(cli_input, sizeof(cli_input));
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
    printf("AerOS - Version 0.01 ALPHA\n");
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
    // the normal terminal colours (light grey on black) for everything after the banner
    console_set_color(BRIGHT, BLACK);
    printf("Type HELP for a list of commands.\n\n");
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

// RUN <code>: run a line of RenLang as a new process, e.g. "run s:hello" or "run print_stack"
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
    // "notes.txt" -> cd + "notes.txt", "/sys/conf.ren" -> "/sys/" + "conf.ren"
    // fails for an empty or invalid name
    if (!fs_split_path(args, dir, sizeof(dir), name, sizeof(name))) {
        printf("Usage: RD <file>\n");
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
    // a directory?
    // fs_delete_d() refuses a directory that still has things in it, and the current directory
    // (or one of its parents), since we would be standing inside it
    if (fs_resolve_dir(args, path, sizeof(path)) && fs_dir_exists(path)) {
        int result = fs_delete_d(path);
        if (result != FS_OK) {
            printf("Can't delete %s: %s\n", path, fs_strerror(result));
        }
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

// the interactive RenLang prompt
static void cli_ren (char* args) {
    (void)args;
    ren_repl();
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
        printf("Usage: EDIT <file>   (e.g. edit hello.ren)\n");
        return;
    }
    editor_open(args);
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

// run "command args" as a RenLang program if command names one ("hello" or "hello.ren"),
// returns false if there is no such program
static bool cli_run_program (const char* command, const char* args) {
    char line[P_INST_MAX];
    strlcpy(line, command, sizeof(line));
    size_t length = strlen(line);
    if (length < 4 || strcasecmp(line + length - 4, ".ren") != 0) {
        strlcat(line, ".ren", sizeof(line));
    }
    if (!program_exists(line)) {
        return false;
    }
    if (*args) {
        strlcat(line, " ", sizeof(line));
        strlcat(line, args, sizeof(line));
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

// accounts (see sys/accounts/auth.h)

#define CLI_PASSWORD_MAX 64

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
    bool ok = auth_check(auth_user, password);
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
    if (auth_exists(args)) {
        printf("%s already exists.\n", args);
        return;
    }
    char password[CLI_PASSWORD_MAX];
    if (cli_ask_new_password(password, sizeof(password))) {
        int result = auth_add(args, password);
        if (result == AUTH_OK) {
            printf("Made the account %s.\n", args);
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
        int result = auth_set_password(auth_user, password);
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
    { "run",      cli_run,            "RUN <CODE>      - RUN CODE AS A PROCESS (e.g. run print_stack, run s:hi)" },
    { "rd",       cli_read_file,      "RD <FILE>       - READ A FILE" },
    { "mkf",      cli_make_file,      "MKF <FILE> [T]  - MAKE A FILE, WITH OPTIONAL TEXT" },
    { "wr",       cli_write_file,     "WR <FILE> <T>   - ADD A LINE OF TEXT TO THE END OF A FILE" },
    { "mkd",      cli_make_directory, "MKD <DIR>       - MAKE A DIRECTORY" },
    { "del",      cli_delete,         "DEL <NAME>      - DELETE A FILE OR AN EMPTY DIRECTORY" },
    { "echo",     cli_echo,           "ECHO <TEXT>     - PRINT TEXT" },
    { "cls",      cli_clear,          "CLS             - CLEAR THE SCREEN" },
    { "mem",      cli_memory,         "MEM             - MEMORY USAGE" },
    { "drv",      cli_drives,         "DRV             - LIST IDE / ATA DRIVES" },
    { "beep",     cli_beep,           "BEEP [HZ]       - PLAY A BEEP ON THE PC SPEAKER" },
    { "reboot",   cli_reboot,         "REBOOT          - RESTART THE COMPUTER" },
    { "shutdown", cli_shutdown,       "SHUTDOWN        - TURN OFF THE COMPUTER" },
    { "res",      cli_resolution,     "RES [WxH]       - SHOW OR CHANGE THE SCREEN RESOLUTION (e.g. RES 1280x720)" },
    { "edit",     cli_edit,          "EDIT <FILE>     - EDIT A FILE (CTRL+S SAVE, CTRL+R RUN A .REN, CTRL+Q QUIT)" },
    { "ren",      cli_ren,            "REN             - THE RENLANG PROMPT, TYPE CODE TO RUN IT" },
    { "whoami",   cli_whoami,         "WHOAMI          - PRINT WHO IS LOGGED IN" },
    { "users",    cli_users,          "USERS           - LIST THE USER ACCOUNTS" },
    { "adduser",  cli_add_user,       "ADDUSER <NAME>  - MAKE A NEW USER ACCOUNT" },
    { "deluser",  cli_delete_user,    "DELUSER <NAME>  - DELETE A USER ACCOUNT" },
    { "passwd",   cli_password,       "PASSWD          - CHANGE YOUR PASSWORD" },
    { "logout",   cli_logout,         "LOGOUT          - LOG OUT, BACK TO THE LOGIN SCREEN" },
    { "",         0,                  "<APP> [ARGS]    - RUN A RENLANG PROGRAM, e.g. HELLO (/bin/hello.ren)" },
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
        // it was a RenLang program (e.g. "hello" runs /bin/hello.ren)
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

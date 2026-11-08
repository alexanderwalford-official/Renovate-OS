// terminal interface

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

#define CLI_INPUT_MAX 128

bool cli_active = false;
static char cli_input[CLI_INPUT_MAX];

// flash a block at the cursor position, times = number of flashes, lc = flashes already done
int flash_pointer (int times, int lc) {
    uint8_t x, y;
    console_get_cursor(&x, &y);
    for (; lc < times; lc++) {
        putc(x, y, console_bg, console_fg, ' '); // solid block
        clck_hang(500);
        putc(x, y, console_fg, console_bg, ' '); // empty
        clck_hang(500);
    }
    return 0;
}

// start the terminal, ent = 0 bypasses the login (accounts are not implemented yet)
int cli_init (int ent) {
    (void)ent; // TODO: login using sys/accounts/auth.h once accounts exist
    cli_active = true;
    cli_rend_main();
    while (cli_active) {
        cli_prompt();
        GetString(cli_input, sizeof(cli_input));
        HandleInput(cli_input);
    }
    return 0;
}

int cli_rend_main (void) {
    console_set_color(WHITE, BLACK);
    console_clear();
    printf("Renovate OS - Version 0.01 ALPHA\n");
    printf("Alexander Walford 2026\n\n");
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
    console_set_color(BRIGHT, BLACK);
    printf("Type HELP for a list of commands.\n\n");
    ClearInputbuffer();
    InputBufferChangeState(1);
    return 0;
}

void cli_prompt (void) {
    console_set_color(BRIGHT, BLACK);
    printf("0/%s: ", cd);
}

// every command takes the rest of the line (trimmed, "" when there is none)

static void cli_help (char* args);

static void cli_list (char* args) {
    (void)args;
    fs_print();
}

static void cli_set_directory (char* args) {
    if (*args == '\0') {
        printf("Usage: SD <directory>\n");
        return;
    }
    if (fs_set_cd(args) != 0) {
        printf("Directory not found: %s\n", args);
    }
}

static void cli_clock (char* args) {
    (void)args;
    rtc_time_t t;
    rtc_read(&t);
    printf("System time: %02u-%02u-%04u %02u:%02u:%02u\n", t.day, t.month, t.year, t.hour, t.minute, t.second);
}

static void cli_uptime (char* args) {
    (void)args;
    uint32_t ms = clck_uptime_ms();
    uint32_t seconds = ms / 1000;
    printf("Time since boot: %uh %02um %02us (%u ms)\n", seconds / 3600, (seconds / 60) % 60, seconds % 60, ms);
}

static void cli_pman (char* args) {
    (void)args;
    printf("> PMAN:\n");
    stack_print();
}

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
    p_exec(pid);
    p_destroy(pid);
}

static void cli_read_file (char* args) {
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (!fs_split_path(args, dir, sizeof(dir), name, sizeof(name))) {
        printf("Usage: RD <file>\n");
        return;
    }
    char* contents = fs_read_f(dir, name);
    if (contents == NULL) {
        printf("File not found: %s\n", args);
        return;
    }
    size_t length = strlen(contents);
    printf((length > 0 && contents[length - 1] == '\n') ? "%s" : "%s\n", contents);
    free(contents);
}

static void cli_make_file (char* args) {
    char* parts[2] = { args, NULL };
    int n = split(args, ' ', parts, 2);
    const char* text = (n > 1) ? StrTrim(parts[1]) : "";
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];

    if (!fs_split_path(parts[0], dir, sizeof(dir), name, sizeof(name))) {
        printf("Usage: MKF <file> [text]\n");
        return;
    }
    int result = fs_create_f(dir, name, text);
    if (result != FS_OK) {
        printf("Could not create %s: %s\n", parts[0], fs_strerror(result));
    }
}

static void cli_write_file (char* args) {
    char* parts[2] = { args, NULL };
    int n = split(args, ' ', parts, 2);
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];

    if (n < 2 || !fs_split_path(parts[0], dir, sizeof(dir), name, sizeof(name))) {
        printf("Usage: WR <file> <text>\n");
        return;
    }
    char* existing = fs_read_f(dir, name);
    if (existing == NULL) {
        printf("File not found: %s\n", parts[0]);
        return;
    }
    bool empty = existing[0] == '\0';
    free(existing);

    // add the text as a new line
    char* text = StrTrim(parts[1]);
    char* line = malloc(strlen(text) + 2);
    if (line == NULL) {
        printf("Out of memory.\n");
        return;
    }
    strcpy(line, empty ? "" : "\n");
    strcat(line, text);

    int result = fs_write_f(dir, name, line, true);
    free(line);
    if (result != FS_OK) {
        printf("Could not write %s: %s\n", parts[0], fs_strerror(result));
    }
}

static void cli_make_directory (char* args) {
    char path[FS_PATH_MAX];
    char parent[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (*args == '\0' || !fs_resolve_dir(args, path, sizeof(path)) || StrCompare(path, "/")) {
        printf("Usage: MKD <directory>\n");
        return;
    }
    // the last part of the path is the directory's name
    fs_parent_path(path, parent, sizeof(parent));
    strlcpy(name, path + strlen(parent), sizeof(name));
    name[strlen(name) - 1] = '\0'; // drop the trailing '/'
    int result = fs_create_d(path, name);
    if (result != FS_OK) {
        printf("Could not create %s: %s\n", path, fs_strerror(result));
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
    // a directory?
    if (fs_resolve_dir(args, path, sizeof(path)) && fs_dir_exists(path)) {
        int result = fs_delete_d(path);
        if (result != FS_OK) {
            printf("Can't delete %s: %s\n", path, fs_strerror(result));
        }
        return;
    }
    printf("Not found: %s\n", args);
}

static void cli_echo (char* args) {
    printf("%s\n", args);
}

static void cli_clear (char* args) {
    (void)args;
    console_clear();
}

static void cli_memory (char* args) {
    (void)args;
    if (boot_multiboot_valid) {
        printf("System memory: %u KB lower, %u KB upper (%u MB total)\n",
               sys_mem_lower_kb, sys_mem_upper_kb, (sys_mem_lower_kb + sys_mem_upper_kb) / 1024);
    }
    printf("Kernel heap: %u / %u bytes used\n", (unsigned int)heap_used(), (unsigned int)heap_total());
    printf("Processes: %d / %d\n", p_cnt, P_STACK_SIZE);
    if (fs_mode == FS_MODE_DISK) {
        // 2 sectors = 1 KB (work in sectors, clusters can be smaller than 1 KB)
        uint32_t free_kb = fat_free_clusters() * fat_vol.sectors_per_cluster / 2;
        uint32_t total_kb = fat_vol.cluster_count * fat_vol.sectors_per_cluster / 2;
        printf("Disk: FAT%d \"%s\", %u KB free of %u KB\n", fat_vol.type, fat_vol.label, free_kb, total_kb);
    } else {
        printf("VFS: %d / %d files, %d / %d directories\n", f_c, FS_MAX_FILES, d_c, FS_MAX_DIRS);
    }
}

static void cli_drives (char* args) {
    (void)args;
    static const char* const bus_names[2] = { "Primary", "Secondary" };
    for (int bus = 0; bus < 2; bus++) {
        for (int slave = 0; slave < 2; slave++) {
            char model[41];
            uint32_t sectors;
            int type = drive_probe_info(bus, slave, model, sizeof(model), &sectors);
            printf("%-9s %-6s : %s %s", bus_names[bus], slave ? "slave" : "master", drive_type_name(type), model);
            if (sectors) {
                printf(" (%u MB)", sectors / 2048);
            }
            if (fs_mode == FS_MODE_DISK && fat_vol.bus == bus && fat_vol.slave == slave) {
                printf(" [mounted]");
            }
            printf("\n");
        }
    }
}

static void cli_beep (char* args) {
    beep(atoi(args));
}

static void cli_reboot (char* args) {
    (void)args;
    sys_reboot();
}

static void cli_shutdown (char* args) {
    (void)args;
    sys_shutdown();
}

// the commands, in the order HELP lists them (names are not case sensitive)
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
};

#define CLI_COMMAND_COUNT (sizeof(cli_commands) / sizeof(cli_commands[0]))

static void cli_help (char* args) {
    (void)args;
    printf("> HELP:\n");
    for (size_t i = 0; i < CLI_COMMAND_COUNT; i++) {
        printf("%s\n", cli_commands[i].help);
    }
}

// read the command and run it
void HandleInput (char* in) {
    char* line = StrTrim(in);
    if (*line == '\0') {
        return; // buffer is empty
    }

    // split the line into the command and its arguments
    char no_args[1] = "";
    char* parts[2] = { line, no_args };
    int n = split(line, ' ', parts, 2);
    char* command = parts[0];
    char* args = (n > 1) ? StrTrim(parts[1]) : no_args;

    unsigned int clears_before = vga_clear_count;

    size_t i = 0;
    while (i < CLI_COMMAND_COUNT && strcasecmp(command, cli_commands[i].name) != 0) {
        i++;
    }
    if (i < CLI_COMMAND_COUNT) {
        cli_commands[i].run(args);
    } else {
        printf("> %s\n", command);
        printf("Unrecognised command, please try again.\n");
    }

    // a program drew a full screen (e.g. the splash or error screen), bring the terminal back
    if (vga_clear_count != clears_before) {
        cli_rend_main();
    }
}

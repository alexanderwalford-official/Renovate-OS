// terminal interface

#ifndef T_INT_H
#define T_INT_H

#include <stdint.h>
#include <stdbool.h>
#include "../../drivers/video/VGA_linear.h"
#include "../../drivers/input/IO.h"
#include "../../drivers/audio/tone.h"
#include "../../drivers/storage/SATA/SATA_scanning.h"
#include "../../lib/primary_definitions.h"
#include "../sys_clck.h"
#include "../f_sys.h"
#include "../boot.h"
#include "../power.h"

#define CLI_INPUT_MAX 128

bool cli_active = false; // true while the terminal is running
char cli_input[CLI_INPUT_MAX];

// prototypes (t_int.h, p_man.h and ren_comp.h use each other)
int flash_pointer (int times, int lc);
int cli_init (int ent);
int cli_rend_main (void);
void cli_prompt (void);
void HandleInput (char* in);

#include "../p_man.h"

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

int cli_rend_main () {
    console_set_color(WHITE, BLACK);
    console_clear();
    printf("Renovate OS - Version 0.01 ALPHA\n");
    printf("Alexander Walford 2026\n\n");
    console_set_color(RED, BLACK);
    printf("[ ! ] You are in VFS mode, your files will not be written.\n");
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

/* ---------------- commands ---------------- */

void cli_help (void) {
    printf("> HELP:\n");
    printf("HELP            - THIS COMMAND\n");
    printf("LA              - LIST ALL FILES AND DIRECTORIES IN THE CURRENT DIRECTORY\n");
    printf("SD <DIR>        - SET THE CURRENT DIRECTORY (e.g. SD /sys, SD .., SD /)\n");
    printf("CLK             - PRINTS THE SYSTEM TIME\n");
    printf("TIME            - PRINTS THE TIME SINCE BOOT\n");
    printf("PMAN            - PRINTS THE PROCESS STACK\n");
    printf("RUN <CODE>      - RUN CODE AS A PROCESS (e.g. run print_stack, run s:hi)\n");
    printf("RD <FILE>       - READ A FILE\n");
    printf("MKF <FILE> [T]  - MAKE A FILE, WITH OPTIONAL TEXT\n");
    printf("WR <FILE> <T>   - ADD A LINE OF TEXT TO THE END OF A FILE\n");
    printf("MKD <DIR>       - MAKE A DIRECTORY\n");
    printf("DEL <NAME>      - DELETE A FILE OR AN EMPTY DIRECTORY\n");
    printf("ECHO <TEXT>     - PRINT TEXT\n");
    printf("CLS             - CLEAR THE SCREEN\n");
    printf("MEM             - MEMORY USAGE\n");
    printf("DRV             - LIST IDE / ATA DRIVES\n");
    printf("BEEP [HZ]       - PLAY A BEEP ON THE PC SPEAKER\n");
    printf("REBOOT          - RESTART THE COMPUTER\n");
    printf("SHUTDOWN        - TURN OFF THE COMPUTER\n");
}

void cli_set_directory (const char* args) {
    if (*args == '\0') {
        printf("Usage: SD <directory>\n");
        return;
    }
    if (fs_set_cd(args) != 0) {
        printf("Directory not found: %s\n", args);
    }
}

void cli_clock (void) {
    rtc_time_t t;
    rtc_read(&t);
    printf("System time: %02u-%02u-%04u %02u:%02u:%02u\n", t.day, t.month, t.year, t.hour, t.minute, t.second);
}

void cli_uptime (void) {
    uint32_t ms = clck_uptime_ms();
    uint32_t seconds = ms / 1000;
    printf("Time since boot: %uh %02um %02us (%u ms)\n", seconds / 3600, (seconds / 60) % 60, seconds % 60, ms);
}

void cli_run (const char* args) {
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

void cli_read_file (const char* args) {
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (!fs_split_path(args, dir, sizeof(dir), name, sizeof(name))) {
        printf("Usage: RD <file>\n");
        return;
    }
    const char* contents = fs_read_f(dir, name);
    if (contents == NULL) {
        printf("File not found: %s\n", args);
        return;
    }
    printf("%s\n", contents);
}

void cli_make_file (char* args) {
    char* parts[2] = { args, NULL };
    int n = split(args, ' ', parts, 2);
    char* text = (n > 1) ? StrTrim(parts[1]) : "";
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];

    if (!fs_split_path(parts[0], dir, sizeof(dir), name, sizeof(name))) {
        printf("Usage: MKF <file> [text]\n");
        return;
    }
    if (fs_find_f(dir, name) >= 0) {
        printf("File already exists: %s\n", parts[0]);
        return;
    }
    if (fs_create_f(dir, name, text) < 0) {
        printf("Could not create %s (directory missing or file system full)\n", parts[0]);
    }
}

void cli_write_file (char* args) {
    char* parts[2] = { args, NULL };
    int n = split(args, ' ', parts, 2);
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];

    if (n < 2 || !fs_split_path(parts[0], dir, sizeof(dir), name, sizeof(name))) {
        printf("Usage: WR <file> <text>\n");
        return;
    }
    const char* existing = fs_read_f(dir, name);
    if (existing == NULL) {
        printf("File not found: %s\n", parts[0]);
        return;
    }
    // start the new text on its own line
    if (existing[0] != '\0') {
        fs_write_f(dir, name, "\n", true);
    }
    if (fs_write_f(dir, name, StrTrim(parts[1]), true) < 0) {
        printf("Out of memory.\n");
    }
}

void cli_make_directory (const char* args) {
    char path[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    if (*args == '\0' || !fs_resolve_dir(args, path, sizeof(path))) {
        printf("Usage: MKD <directory>\n");
        return;
    }
    if (fs_find_d(path) >= 0) {
        printf("Directory already exists: %s\n", path);
        return;
    }
    // the last part of the path is the directory's name
    char parent[FS_PATH_MAX];
    fs_parent_path(path, parent, sizeof(parent));
    strlcpy(name, path + strlen(parent), sizeof(name));
    name[strlen(name) - 1] = '\0'; // drop the trailing '/'
    if (fs_create_d(path, name) < 0) {
        printf("Could not create %s (parent missing or file system full)\n", path);
    }
}

void cli_delete (const char* args) {
    char dir[FS_PATH_MAX];
    char name[FS_NAME_MAX];
    char path[FS_PATH_MAX];

    if (*args == '\0') {
        printf("Usage: DEL <file or directory>\n");
        return;
    }
    // a file?
    if (fs_split_path(args, dir, sizeof(dir), name, sizeof(name)) && fs_find_f(dir, name) >= 0) {
        fs_delete_f(dir, name);
        return;
    }
    // a directory?
    if (fs_resolve_dir(args, path, sizeof(path)) && fs_find_d(path) >= 0) {
        if (fs_delete_d(path) != 0) {
            printf("Can't delete %s (not empty, or it is the current directory)\n", path);
        }
        return;
    }
    printf("Not found: %s\n", args);
}

void cli_memory (void) {
    if (boot_multiboot_valid) {
        printf("System memory: %u KB lower, %u KB upper (%u MB total)\n",
               sys_mem_lower_kb, sys_mem_upper_kb, (sys_mem_lower_kb + sys_mem_upper_kb) / 1024);
    }
    printf("Kernel heap: %u / %u bytes used\n", (unsigned int)heap_used(), (unsigned int)heap_total());
    printf("Processes: %d / %d   Files: %d / %d   Directories: %d / %d\n",
           p_cnt, P_STACK_SIZE, f_c, FS_MAX_FILES, d_c, FS_MAX_DIRS);
}

void cli_drives (void) {
    const char* bus_names[2] = { "Primary", "Secondary" };
    for (int bus = 0; bus < 2; bus++) {
        for (int slave = 0; slave < 2; slave++) {
            char model[41];
            int type = drive_probe(bus, slave, model, sizeof(model));
            printf("%-9s %-6s : %s %s\n", bus_names[bus], slave ? "slave" : "master", drive_type_name(type), model);
        }
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

    if (strcasecmp(command, "help") == 0) {
        cli_help();
    }
    else if (strcasecmp(command, "la") == 0) {
        fs_print();
    }
    else if (strcasecmp(command, "sd") == 0) {
        cli_set_directory(args);
    }
    else if (strcasecmp(command, "clk") == 0) {
        cli_clock();
    }
    else if (strcasecmp(command, "time") == 0) {
        cli_uptime();
    }
    else if (strcasecmp(command, "pman") == 0) {
        printf("> PMAN:\n");
        stack_print();
    }
    else if (strcasecmp(command, "run") == 0) {
        cli_run(args);
    }
    else if (strcasecmp(command, "rd") == 0) {
        cli_read_file(args);
    }
    else if (strcasecmp(command, "mkf") == 0) {
        cli_make_file(args);
    }
    else if (strcasecmp(command, "wr") == 0) {
        cli_write_file(args);
    }
    else if (strcasecmp(command, "mkd") == 0) {
        cli_make_directory(args);
    }
    else if (strcasecmp(command, "del") == 0) {
        cli_delete(args);
    }
    else if (strcasecmp(command, "echo") == 0) {
        printf("%s\n", args);
    }
    else if (strcasecmp(command, "cls") == 0) {
        console_clear();
    }
    else if (strcasecmp(command, "mem") == 0) {
        cli_memory();
    }
    else if (strcasecmp(command, "drv") == 0) {
        cli_drives();
    }
    else if (strcasecmp(command, "beep") == 0) {
        beep(atoi(args));
    }
    else if (strcasecmp(command, "reboot") == 0) {
        sys_reboot();
    }
    else if (strcasecmp(command, "shutdown") == 0) {
        sys_shutdown();
    }
    else {
        // unrecognised
        printf("> %s\n", command);
        printf("Unrecognised command, please try again.\n");
    }

    // a program drew a full screen (e.g. the splash or error screen), bring the terminal back
    if (vga_clear_count != clears_before) {
        cli_rend_main();
    }
}

#endif /* T_INT_H */

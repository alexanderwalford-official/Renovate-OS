/*
    cmd_system.c - the terminal's commands about the computer itself

        ECH, CLS             print some text back, clear the screen
        CLK, UP              the date and time, and how long AerOS has been running
        MEM, DRV             the memory and storage in use, and the drives plugged in
        BP, PLY              a beep, and playing a sound or a video (sys/sound.h, sys/video.h)
        RES                  the screen resolution: show it, change it, save it for next time
        RT                   RT mode's real-time tasks (sys/rt.h)
        RBT, OFF             restart the computer, or turn it off

    Most of these only ask another part of AerOS and print what it says, so each one names
    the file that does the real work. Each command is a row in the table in t_int.c, and
    commands.h says how the files of the terminal fit together.
*/

#include "commands.h"
#include <stdint.h>
#include <stddef.h>
#include "../../lib/primary_definitions.h"
#include "../../drivers/video/VGA_linear.h"
#include "../../drivers/video/bga.h"
#include "../../drivers/input/IO.h"
#include "../../drivers/audio/tone.h"
#include "../../drivers/storage/ata.h"
#include "../sys_clck.h"
#include "../fat.h"
#include "../f_sys.h"
#include "../boot.h"
#include "../power.h"
#include "../p_man.h"
#include "../sound.h"
#include "../video.h"
#include "../ui.h"
#include "../explain.h"
#include "../appmode.h"
#include "../rt.h"


// =================================================================================================
//  The system
// =================================================================================================

// ECH <TEXT>: print the text back
void cli_echo (char* arguments) {
    prtf("%s\n", arguments);
}

// CLS: clear the screen.
// This uses console_clear(), not clear(), so vga_clear_count doesn't change and HandleInput
// doesn't redraw the banner: you get an empty screen with just the prompt
void cli_clear (char* arguments) {
    (void)arguments;
    console_clear();
}

// CLK: the date and time from the computer's battery backed clock (the CMOS clock).
// In an emulator this is usually the host computer's time, often in UTC
void cli_clock (char* arguments) {
    (void)arguments;
    rtc_time_t now;
    rtc_read(&now);
    // day-month-year, the British way round
    prtf("System time: %02u-%02u-%04u %02u:%02u:%02u\n",
           now.day, now.month, now.year, now.hour, now.minute, now.second);
}

// UP: how long AerOS has been running, counted by the system timer (1000 ticks a second)
void cli_uptime (char* arguments) {
    (void)arguments;
    uint32_t milliseconds = clkuptms();
    uint32_t seconds = milliseconds / 1000;

    uint32_t hours = seconds / 3600;
    uint32_t minutes = (seconds / 60) % 60; // 0 to 59
    seconds = seconds % 60;                 // 0 to 59
    prtf("Time since boot: %uh %02um %02us (%u ms)\n", hours, minutes, seconds, milliseconds);
}

// MEM: how much memory and storage is in use
void cli_memory (char* arguments) {
    (void)arguments;

    // the computer's memory is only known if GRUB told us about it (see sys/boot.c)
    if (boot_multiboot_valid) {
        uint32_t total_mb = (sys_mem_lower_kb + sys_mem_upper_kb) / 1024;
        prtf("System memory: %u KB lower, %u KB upper (%u MB total)\n",
               sys_mem_lower_kb, sys_mem_upper_kb, total_mb);
    }
    prtf("Kernel heap: %u / %u bytes used\n", (unsigned int)heap_used(), (unsigned int)heap_total());
    prtf("Processes: %d / %d\n", p_cnt, P_STACK_SIZE);

    // what app mode (sys/appmode.h) did for the last program that ran
    const appmode_t* mode = appmode_now();
    if (mode->ran) {
        char uses[64];
        appmode_describe(mode->uses, uses, sizeof(uses));
        prtf("App mode: %u KB put away for the last program, which used\n", (unsigned int)(mode->freed / 1024));
        prtf("          %s\n", uses);
    } else {
        prtf("App mode: no program has run yet\n");
    }

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
void cli_drives (char* arguments) {
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

// BP [HZ]: a beep on the PC speaker. atoi() gives 0 for nothing (or not a number), and
// beep(0) is the standard beep
void cli_beep (char* arguments) {
    beep(atoi(arguments));
}

// RBT: restart the computer (never comes back)
void cli_reboot (char* arguments) {
    (void)arguments;
    sys_reboot();
}

// OFF: turn the computer off, or show "It is now safe to turn off your computer" if it
// can't be turned off by software (never comes back)
void cli_shutdown (char* arguments) {
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

    // 1024 bytes for GRUB, plus one for the '\0' the file functions need to find the end.
    // "x32" asks for 32 bits per pixel. sfmt returns how much it wrote, and the '#'s fill
    // everything from there up to byte 1024
    char block[1025];
    int used = sfmt(block, sizeof(block), "# GRUB Environment Block\naeros_mode=%ux%ux32\n", width, height);
    memset(block + used, '#', (size_t)(1024 - used));
    block[1024] = '\0';

    // replace the whole file (append is false) if it is there already, or make it
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
    // strtol reads a number and sets `end` to the first character after it: here, the 'x'
    char* end;
    *width = strtol(text, &end, 10);
    *height = 0; // stays 0 without an 'x', so "1280" on its own fails the size check below
    if (*end == 'x' || *end == 'X') {
        *height = strtol(end + 1, &end, 10);
    }

    // turned away: anything after the size ("1280x720p"), and sizes too small or too big to use
    bool nothing_after = *end == '\0';
    bool big_enough = *width >= 640 && *height >= 400;
    bool small_enough = *width <= 4096 && *height <= 4096;
    return nothing_after && big_enough && small_enough;
}

// RES [WxH]: show the screen resolution, or change it, e.g. RES 1280x720
void cli_resolution (char* arguments) {
    vga_framebuffer_t screen;
    bool graphics = vga_framebuffer_info(&screen);

    // no size given: say what it is now
    if (arguments[0] == '\0') {
        if (graphics) {
            prtf("Screen: %ux%u, %u bits per pixel\n", screen.width, screen.height, (unsigned int)screen.bpp);
        } else {
            prtf("Screen: VGA text mode\n");
        }
        prtf("Change it with %s <width>x<height>, e.g. %s 1280x720\n",
             command_name(cli_resolution), command_name(cli_resolution));
        prtf("Common sizes: 640x480 800x600 1024x768 1280x720 1280x1024 1600x900 1920x1080\n");
        return;
    }

    long width, height;
    if (!parse_resolution(arguments, &width, &height)) {
        show_usage("at least 640x400, e.g. 1280x720");
        return;
    }

    // try both: changing it now (only some graphics cards can) and saving it for next time
    bool changed = bga_set_mode((uint32_t)width, (uint32_t)height);
    bool saved = save_resolution((uint32_t)width, (uint32_t)height);

    // ask the screen again rather than trust what was asked for, so the message says what it
    // really is
    if (changed) {
        vga_framebuffer_info(&screen);
        prtf("The screen is now %ux%u.\n", screen.width, screen.height);
    }
    // then say which of the two worked
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
// PLY shows one status line. While the file is being read (and an MP3 decoded) it is a
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

    // this is called far more often than the line needs drawing, and drawing it every time
    // would take time away from the sound. 0 means it hasn't been drawn yet
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
    // only take a key if one is waiting: getk() would wait for one, and the sound with it.
    // Keys other than Esc are taken and thrown away
    return kb_haschar() && getk() == KEY_ESCAPE;
}

// PLY <FILE>: play a WAV or MP3 file on the sound card, or an .mpg video full screen
void cli_play (char* arguments) {
    if (arguments[0] == '\0') {
        show_usage("a .wav, .mp3 or .mpg file");
        return;
    }

    // a video plays full screen, with its own controls (sys/video.h). The terminal redraws
    // itself afterwards, like after any program that took over the screen
    if (video_is_video(arguments)) {
        int result = video_play_file(arguments);
        if (result != VIDEO_OK) {
            explain_say(explain_video_problem(result), arguments,
                        "Can't play %s: %s", arguments, video_strerror(result));
        }
        return;
    }

    // a sound: get the status line ready for play_should_stop(), and turn the "Reading"
    // spinner until the sound card starts. sound_play_file only comes back once it has finished
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
        explain_say(explain_sound_problem(result), arguments,
                    "Can't play %s: %s", arguments, sound_strerror(result));
    }
}


// =================================================================================================
//  RT mode's real-time tasks (sys/rt.h)
// =================================================================================================

// RT [STRESS]: the real-time tasks, how often they run and the longest they have taken.
// RT STRESS adds one that breaks its budget on purpose, so the watchdog can be seen stopping it
void cli_realtime (char* arguments) {
    if (!rt_on()) {
        prtf("RT mode is off. To try it, press X on the splash screen as AerOS starts, then 6.\n");
        return;
    }
    // RT STRESS: rt_add_stress refuses if the tasks there are already use too much of the
    // processor for one more to fit
    if (strcasecmp(arguments, "stress") == 0) {
        int result = rt_add_stress();
        prtf(result == RT_OK ? "Added \"stress\": it overruns its budget every time, watch the watchdog stop it.\n"
                             : "Couldn't add it: the tasks would no longer all meet their deadlines.\n");
        return;
    }
    if (arguments[0] != '\0') {
        show_usage("RT on its own, or RT STRESS");
        return;
    }

    // RT on its own: a table of the tasks. EVERY is how often each runs, BUDGET the longest it
    // is allowed, WORST the longest it has really taken, OVER how often it went past its budget
    prtf("Real-time tasks, most urgent first:\n");
    prtf("  %-10s %7s %8s %8s %8s %6s\n", "TASK", "EVERY", "BUDGET", "WORST", "RUNS", "OVER");
    for (int i = 0; i < rt_count(); i++) {
        const rt_task_t* task = rt_task(i);
        prtf("  %-10s %5ums %6uus %6uus %8u %6u%s\n", task->name, task->period_ms, task->budget_us,
             task->worst_us, task->runs, task->overruns, task->stopped ? "  stopped by the watchdog" : "");
    }
    // both in tenths of a percent (whole numbers, no floating point), so 123 is printed as 12.3%
    uint32_t load = rt_load();
    uint32_t limit = rt_limit(rt_count());
    prtf("They need %u.%u%% of the processor, of the %u.%u%% that keeps every deadline safe.\n",
         load / 10, load % 10, limit / 10, limit % 10);
    prtf("Heartbeats so far: %u (one every 10ms since RT mode started)\n", rt_heartbeats());
}

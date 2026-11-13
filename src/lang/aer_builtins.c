/*
    aer_builtins.c
    The table of every function an AerLang program can call (aer_builtins[] below), and what
    goes with it: the constants every program has (KEY_UP, BLACK, PI...), which built-ins need
    which part of AerOS (for app mode), and the tidying up when a program ends.

    The built-ins themselves are in the bi_*.c files next to this one, one file for each topic:
    builtins.h says which file holds what, and how to add one. The table stays here in one
    piece, as the index of everything AerLang can call.
*/

#include "aer_internal.h"
#include "builtins.h"
#include "../lib/aer_string.h"
#include "../lib/aer_stdlib.h"
#include "../lib/aer_math.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/video/gfx.h"
#include "../drivers/input/IO.h"
#include "../drivers/input/mouse.h"
#include "../sys/sys_clck.h"
#include "../sys/mixer.h"
#include "../sys/appmode.h"

// One row of the table: the name programs call it by, the C function that does it, and how
// many arguments it takes (fewest, most). #fn turns the function into its name as a string,
// "bi_print", so WHY and SRC can find the C code behind an AerLang function
#define BUILTIN(name, fn, min_args, max_args) { name, fn, min_args, max_args, #fn }

// every built-in AerLang knows. When a program starts, the interpreter makes each name a global
// variable holding the row's position in this table (a V_NATIVE value), so a call finds its row
// straight away and checks the number of arguments before calling the function
const aer_builtin_t aer_builtins[] = {
    // console
    BUILTIN("print", bi_print, 0, AER_MAX_ARGS),
    BUILTIN("write", bi_write, 0, AER_MAX_ARGS),
    BUILTIN("input", bi_input, 0, 1),
    BUILTIN("inpw", bi_input_secret, 0, 1),
    BUILTIN("cls", bi_cls, 0, 0),
    BUILTIN("color", bi_color, 1, 2),
    BUILTIN("at", bi_at, 2, 2),
    BUILTIN("key", bi_key, 0, 0),
    BUILTIN("haskey", bi_haskey, 0, 0),
    // strings, conversions and maths
    BUILTIN("len", bi_len, 1, 1),
    BUILTIN("str", bi_str, 1, 1),
    BUILTIN("int", bi_int, 1, 1),
    BUILTIN("float", bi_float, 1, 1),
    BUILTIN("sqrt", bi_sqrt, 1, 1),
    BUILTIN("sin", bi_sin, 1, 1),
    BUILTIN("cos", bi_cos, 1, 1),
    BUILTIN("atan2", bi_atan2, 2, 2),
    BUILTIN("floor", bi_floor, 1, 1),
    BUILTIN("round", bi_round, 1, 1),
    BUILTIN("abs", bi_abs, 1, 1),
    BUILTIN("chr", bi_chr, 1, 1),
    BUILTIN("ord", bi_ord, 1, 1),
    BUILTIN("sub", bi_sub, 2, 3),
    BUILTIN("find", bi_find, 2, 2),
    BUILTIN("upper", bi_upper, 1, 1),
    BUILTIN("lower", bi_lower, 1, 1),
    BUILTIN("split", bi_split, 1, 2),
    BUILTIN("join", bi_join, 1, 2),
    // lists and bytes
    BUILTIN("list", bi_list, 1, 2),
    BUILTIN("push", bi_push, 2, 2),
    BUILTIN("pop", bi_pop, 1, 1),
    BUILTIN("insert", bi_insert, 3, 3),
    BUILTIN("remove", bi_remove, 2, 2),
    BUILTIN("bytes", bi_bytes, 1, 1),
    // files and encrypted files
    BUILTIN("rdf", bi_read_file, 1, 1),
    BUILTIN("rdb", bi_read_bytes, 1, 1),
    BUILTIN("wrf", bi_write_file, 2, 2),
    BUILTIN("apf", bi_append_file, 2, 2),
    BUILTIN("rdat", bi_read_at, 3, 3),
    BUILTIN("wrat", bi_write_at, 3, 3),
    BUILTIN("trunc", bi_truncate, 2, 2),
    BUILTIN("isf", bi_file_exists, 1, 1),
    BUILTIN("delf", bi_delete_file, 1, 1),
    BUILTIN("enc", bi_encrypt_file, 2, 2),
    BUILTIN("dec", bi_decrypt_file, 2, 2),
    BUILTIN("isenc", bi_is_encrypted, 1, 1),
    BUILTIN("rdenc", bi_read_encrypted, 2, 2),
    BUILTIN("la", bi_list_files, 0, 1),
    BUILTIN("cwd", bi_cwd, 0, 0),
    // system, text screen animation, network and sound
    BUILTIN("sleep", bi_sleep, 1, 1),
    BUILTIN("time", bi_time, 0, 0),
    BUILTIN("sysinfo", bi_system_info, 0, 0),
    BUILTIN("big", bi_big_text, 3, 5),
    BUILTIN("progress", bi_progress, 5, 6),
    BUILTIN("spinner", bi_spinner, 2, 4),
    BUILTIN("frame", bi_frame, 1, 1),
    BUILTIN("queue", bi_queue, 1, 1),
    BUILTIN("cmd", bi_cmd, 1, 1),
    BUILTIN("nsend", bi_net_send, 3, 3),
    BUILTIN("nrecv", bi_net_receive, 2, 2),
    BUILTIN("dbopen", bi_dbopen, 1, 1),
    BUILTIN("dbrun", bi_dbrun, 2, 3),
    BUILTIN("dbquery", bi_dbquery, 2, 3),
    BUILTIN("dbclose", bi_dbclose, 1, 1),
    BUILTIN("appmode", bi_appmode, 0, 0),
    BUILTIN("random", bi_random, 1, 1),
    BUILTIN("beep", bi_beep, 0, 1),
    BUILTIN("play", bi_play, 1, 1),
    BUILTIN("exit", bi_exit, 0, 0),
    // graphics
    BUILTIN("gopen", bi_gfx_open, 0, 0),
    BUILTIN("gclose", bi_gfx_close, 0, 0),
    BUILTIN("gw", bi_gfx_width, 0, 0),
    BUILTIN("gh", bi_gfx_height, 0, 0),
    BUILTIN("gclear", bi_gfx_clear, 0, 1),
    BUILTIN("rgb", bi_rgb, 3, 3),
    BUILTIN("pixel", bi_pixel, 3, 3),
    BUILTIN("rect", bi_rect, 5, 5),
    BUILTIN("line", bi_line, 5, 5),
    BUILTIN("text", bi_text, 4, 5),
    BUILTIN("image", bi_image, 3, 5),
    BUILTIN("imgsz", bi_image_size, 1, 1),
    // for games, and video
    BUILTIN("gbuf", bi_gbuf, 1, 1),
    BUILTIN("gshow", bi_gshow, 0, 0),
    BUILTIN("keydown", bi_keydown, 1, 1),
    BUILTIN("mouse", bi_mouse, 0, 0),
    BUILTIN("mousemove", bi_mousemove, 0, 0),
    BUILTIN("mousedown", bi_mousedown, 1, 1),
    BUILTIN("sload", bi_sload, 1, 1),
    BUILTIN("splay", bi_splay, 1, 3),
    BUILTIN("sstop", bi_sstop, 0, 1),
    BUILTIN("isply", bi_isply, 1, 1),
    BUILTIN("video", bi_video, 1, 1),
};

const int aer_builtin_count = sizeof(aer_builtins) / sizeof(aer_builtins[0]);

// ---- app mode: which built-ins belong to which part of AerOS (sys/appmode.h) ---------------------

// for each part of AerOS that app mode can put away, the built-ins that need it. While a program
// that names none of a part's built-ins runs, that part can be put away. The longest list (graphics) has
// 14 names, so 16 leaves room for the NULL and one more
static const struct {
    uint32_t part;
    const char* names[24];   // ends with NULL
} parts[] = {
    { APP_GRAPHICS, { "gopen", "gclose", "gw", "gh", "gclear", "rgb", "pixel", "rect", "line",
                      "text", "image", "imgsz", "gbuf", "gshow", NULL } },
    { APP_SOUND,    { "beep", "play", "sload", "splay", "sstop", "isply", NULL } },
    { APP_VIDEO,    { "video", NULL } },
    { APP_FILES,    { "rdf", "rdb", "wrf", "apf", "isf", "delf", "la", "cwd", "enc", "dec",
                      "isenc", "rdenc", "rdat", "wrat", "trunc", "dbopen", "dbrun", "dbquery",
                      "dbclose", NULL } },
    { APP_NETWORK,  { "nsend", "nrecv", NULL } },
};

// the part a built-in's name belongs to, 0 for the ones every program has (print, len...)
static uint32_t part_of (const char* name) {
    for (size_t p = 0; p < sizeof(parts) / sizeof(parts[0]); p++) {
        for (int i = 0; parts[p].names[i] != NULL; i++) {
            if (scmp(parts[p].names[i], name) == 0) {
                return parts[p].part;
            }
        }
    }
    return 0;
}

// which parts of AerOS (APP_ bits) a program's tokens name, see aer_internal.h
uint32_t aer_profile (const token_t* tokens, int count) {
    // every name in the program, called or not: "let f = play" then "f(...)" still names play.
    // (A variable that happens to share a built-in's name counts too, which only means a part
    // is kept that could have been put away: never the other way round)
    uint32_t uses = 0;
    for (int i = 0; i < count; i++) {
        if (tokens[i].type == T_NAME) {
            // each part is its own bit, so |= adds this one's part to the set (0 adds nothing)
            uses |= part_of(tokens[i].text);
        }
    }
    return uses;
}

// the C function's name behind a built-in, "gopen" -> "bi_gfx_open" (see aerlang.h)
const char* aer_builtin_function (const char* name) {
    // strcasecmp ignores capitals, so WHY GOPEN finds gopen
    for (int i = 0; i < aer_builtin_count; i++) {
        if (strcasecmp(aer_builtins[i].name, name) == 0) {
            return aer_builtins[i].function;
        }
    }
    return NULL;
}

// make KEY_UP, BLACK, PI and the other constants global variables, and seed random()
void aer_define_constants (void) {
    // the key codes are what key() gives back, and the colours are the 16 text colours (0 to 15).
    // Enter, Backspace, Tab and Space are plain characters, so they are written as such
    static const struct { const char* name; int32_t value; } constants[] = {
        { "KEY_UP", KEY_UP }, { "KEY_DOWN", KEY_DOWN }, { "KEY_LEFT", KEY_LEFT },
        { "KEY_RIGHT", KEY_RIGHT }, { "KEY_HOME", KEY_HOME }, { "KEY_END", KEY_END },
        { "KEY_DELETE", KEY_DELETE }, { "KEY_ESCAPE", KEY_ESCAPE }, { "KEY_ENTER", '\n' },
        { "KEY_BACKSPACE", '\b' }, { "KEY_TAB", '\t' }, { "KEY_PGUP", KEY_PGUP }, { "KEY_PGDN", KEY_PGDN },
        { "KEY_SHIFT", KEY_SHIFT }, { "KEY_CTRL", KEY_CTRL }, { "KEY_ALT", KEY_ALT }, { "KEY_SPACE", ' ' },
        { "MOUSE_LEFT", MOUSE_LEFT }, { "MOUSE_RIGHT", MOUSE_RIGHT }, { "MOUSE_MIDDLE", MOUSE_MIDDLE },
        { "BLACK", BLACK }, { "DARK_BLUE", DARK_BLUE }, { "GREEN", GREEN }, { "CYAN", CYAN },
        { "RED", RED }, { "PURPLE", PURPLE }, { "ORANGE", ORANGE }, { "BROWN", BROWN }, { "BRIGHT", BRIGHT },
        { "DARK_GRAY", DARK_GRAY }, { "LIGHT_BLUE", LIGHT_BLUE }, { "LIGHT_GREEN", LIGHT_GREEN },
        { "LIGHT_CYAN", LIGHT_CYAN }, { "LIGHT_RED", LIGHT_RED }, { "LIGHT_PURPLE", LIGHT_PURPLE },
        { "YELLOW", YELLOW }, { "WHITE", WHITE },
    };
    for (size_t i = 0; i < sizeof(constants) / sizeof(constants[0]); i++) {
        aer_define_global(constants[i].name, aer_int(constants[i].value));
    }
    aer_define_global("PI", aer_float(AER_PI)); // half a turn, in radians
    srand(clkuptms()); // a different random() sequence every run
}

// code that switched to graphics (gopen) and didn't switch back gives the screen back
void aer_give_screen_back (void) {
    if (aer->graphics) {
        gfx_end();
        aer->graphics = false;
    }
}

// a program that ends (or crashes) in graphics mode gets the terminal back automatically
void aer_builtins_finish (void) {
    mixer_stop_all(); // sounds from splay() stop with the program (their memory is about to go)
    mouse_stop();     // the mouse is only for programs: the terminal goes back to not having it
    database_finish(); // databases a program left open are closed
    aer_give_screen_back();
}

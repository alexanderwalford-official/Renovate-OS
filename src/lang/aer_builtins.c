/*
    aer_builtins.c
    The functions every AerLang program can call: printing and input, strings, lists, byte
    buffers, files, encrypted files, sound, graphics and a few system services.

    Each one gets its arguments as an array of values, which it must not release (the caller
    still owns them), and hands back a new value that the caller will own. A wrong argument is
    reported with aer_error(), which never returns, so the code after a check can trust it.

    Adding a builtin:
      1. write  static value_t bi_<name> (int argc, value_t* argv, int line)  in the right section,
         using int_arg / str_arg / list_arg / file_arg to check and unpack the arguments
      2. add a line to aer_builtins[] at the bottom: { "name", bi_<name>, fewest args, most args }
         (the interpreter checks the argument count before calling it)
      3. add it to docs/AERLANG.md
*/

#include "aer_internal.h"
#include "../lib/aer_string.h"
#include "../lib/aer_stdlib.h"
#include "../lib/aer_stdio.h"
#include "../lib/aer_ctype.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/video/gfx.h"
#include "../drivers/input/IO.h"
#include "../drivers/audio/tone.h"
#include "../sys/sys_clck.h"
#include "../sys/f_sys.h"
#include "../sys/fcrypt.h"
#include "../sys/sound.h"
#include "../sys/sysinfo.h"
#include "../sys/ui.h"
#include "../sys/p_man.h"
#include "../lib/crypto.h"

// argument checks

static int32_t int_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type != V_INT) {
        aer_error(line, "%s() needs a number for argument %d, not a %s", fn, i + 1, aer_type_name(argv[i]));
    }
    return argv[i].as.i;
}

static aer_object_t* str_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type != V_STR) {
        aer_error(line, "%s() needs a string for argument %d, not a %s", fn, i + 1, aer_type_name(argv[i]));
    }
    return argv[i].as.obj;
}

static aer_object_t* list_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type != V_LIST) {
        aer_error(line, "%s() needs a list for argument %d, not a %s", fn, i + 1, aer_type_name(argv[i]));
    }
    return argv[i].as.obj;
}

// split a path typed by a program into directory and file name
static bool file_arg (value_t* argv, int i, int line, const char* fn, char* dir, char* name) {
    aer_object_t* path = str_arg(argv, i, line, fn);
    return fs_split_path(path->data.chars, dir, FS_PATH_MAX, name, FS_NAME_MAX);
}

// console

static void print_values (int argc, value_t* argv) {
    for (int i = 0; i < argc; i++) {
        if (i > 0) {
            console_putchar_raw(' ');
        }
        aer_print_value(argv[i]);
    }
}

static value_t bi_print (int argc, value_t* argv, int line) {
    (void)line;
    print_values(argc, argv);
    console_putchar('\n');
    return aer_nil();
}

static value_t bi_write (int argc, value_t* argv, int line) {
    (void)line;
    print_values(argc, argv);
    console_update_cursor();
    return aer_nil();
}

static value_t bi_input (int argc, value_t* argv, int line) {
    char buffer[256];
    if (argc > 0) {
        aer_print_value(argv[0]);
    }
    (void)line;
    getln(buffer, sizeof(buffer));
    return aer_string(buffer, (uint32_t)slen(buffer));
}

// like input(), but shows * for each character (for passwords)
static value_t bi_input_secret (int argc, value_t* argv, int line) {
    char buffer[256];
    if (argc > 0) {
        aer_print_value(argv[0]);
    }
    (void)line;
    GetSecret(buffer, sizeof(buffer));
    value_t s = aer_string(buffer, (uint32_t)slen(buffer));
    wipe(buffer, sizeof(buffer));
    return s;
}

static value_t bi_cls (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    console_clear();
    return aer_nil();
}

static value_t bi_color (int argc, value_t* argv, int line) {
    int32_t fg = int_arg(argv, 0, line, "color");
    int32_t bg = argc > 1 ? int_arg(argv, 1, line, "color") : (int32_t)console_bg;
    colour((enum color)(fg & 15), (enum color)(bg & 15));
    return aer_nil();
}

static value_t bi_at (int argc, value_t* argv, int line) {
    (void)argc;
    console_set_cursor((uint8_t)int_arg(argv, 0, line, "at"), (uint8_t)int_arg(argv, 1, line, "at"));
    return aer_nil();
}

static value_t bi_key (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    return aer_int(getk());
}

static value_t bi_haskey (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    return aer_bool(kb_haschar());
}

// strings and conversions

static value_t bi_len (int argc, value_t* argv, int line) {
    (void)argc;
    if (argv[0].type != V_STR && argv[0].type != V_LIST && argv[0].type != V_BYTES) {
        aer_error(line, "len() needs a string, list or bytes, not a %s", aer_type_name(argv[0]));
    }
    return aer_int((int32_t)argv[0].as.obj->length);
}

static value_t bi_str (int argc, value_t* argv, int line) {
    (void)argc; (void)line;
    return aer_to_string(argv[0]);
}

static value_t bi_int (int argc, value_t* argv, int line) {
    (void)argc; (void)line;
    switch (argv[0].type) {
        case V_INT: return argv[0];
        case V_BOOL: return aer_int(argv[0].as.i);
        case V_STR: {
            char* end;
            const char* text = argv[0].as.obj->data.chars;
            long value = strtol(text, &end, 0);
            while (isspace((unsigned char)*end)) {
                end++;
            }
            if (end == text || *end != '\0') {
                return aer_nil(); // not a number
            }
            return aer_int((int32_t)value);
        }
        default: return aer_nil();
    }
}

static value_t bi_chr (int argc, value_t* argv, int line) {
    (void)argc;
    char c = (char)int_arg(argv, 0, line, "chr");
    return aer_string(&c, 1);
}

static value_t bi_ord (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* s = str_arg(argv, 0, line, "ord");
    if (s->length == 0) {
        aer_error(line, "ord() needs a string with at least one character");
    }
    return aer_int((uint8_t)s->data.chars[0]);
}

static value_t bi_sub (int argc, value_t* argv, int line) {
    aer_object_t* s = str_arg(argv, 0, line, "sub");
    int32_t start = int_arg(argv, 1, line, "sub");
    int32_t count = argc > 2 ? int_arg(argv, 2, line, "sub") : (int32_t)s->length;
    if (start < 0) {
        start = 0;
    }
    if ((uint32_t)start > s->length) {
        start = (int32_t)s->length;
    }
    if (count < 0) {
        count = 0;
    }
    if ((uint32_t)count > s->length - (uint32_t)start) {
        count = (int32_t)(s->length - (uint32_t)start);
    }
    return aer_string(s->data.chars + start, (uint32_t)count);
}

static value_t bi_find (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* s = str_arg(argv, 0, line, "find");
    aer_object_t* needle = str_arg(argv, 1, line, "find");
    char* found = strstr(s->data.chars, needle->data.chars);
    return aer_int(found ? (int32_t)(found - s->data.chars) : -1);
}

static value_t change_case (value_t* argv, int line, const char* fn, bool upper) {
    aer_object_t* s = str_arg(argv, 0, line, fn);
    value_t result = aer_string(s->data.chars, s->length);
    for (uint32_t i = 0; i < s->length; i++) {
        char* c = &result.as.obj->data.chars[i];
        *c = (char)(upper ? toupper((unsigned char)*c) : tolower((unsigned char)*c));
    }
    return result;
}

static value_t bi_upper (int argc, value_t* argv, int line) {
    (void)argc;
    return change_case(argv, line, "upper", true);
}

static value_t bi_lower (int argc, value_t* argv, int line) {
    (void)argc;
    return change_case(argv, line, "lower", false);
}

// split("a,b,c", ",") is ["a", "b", "c"], without a separator it splits on spaces
static value_t bi_split (int argc, value_t* argv, int line) {
    aer_object_t* s = str_arg(argv, 0, line, "split");
    const char* sep = argc > 1 ? str_arg(argv, 1, line, "split")->data.chars : 0;
    size_t sep_length = sep ? slen(sep) : 0;
    if (sep && sep_length == 0) {
        aer_error(line, "split() needs a separator that isn't empty");
    }

    value_t list = aer_list(4);
    const char* p = s->data.chars;
    const char* end = p + s->length;
    if (!sep) {
        while (p < end) {
            while (p < end && *p == ' ') {
                p++;
            }
            const char* start = p;
            while (p < end && *p != ' ') {
                p++;
            }
            if (p > start) {
                aer_list_push(list.as.obj, aer_string(start, (uint32_t)(p - start)));
            }
        }
        return list;
    }
    for (;;) {
        const char* hit = strstr(p, sep);
        if (hit == 0) {
            aer_list_push(list.as.obj, aer_string(p, (uint32_t)(end - p)));
            return list;
        }
        aer_list_push(list.as.obj, aer_string(p, (uint32_t)(hit - p)));
        p = hit + sep_length;
    }
}

static value_t bi_join (int argc, value_t* argv, int line) {
    aer_object_t* list = list_arg(argv, 0, line, "join");
    const char* sep = argc > 1 ? str_arg(argv, 1, line, "join")->data.chars : "";
    uint32_t sep_length = (uint32_t)slen(sep);

    // turn every item into a string first, so the total length is known
    value_t* pieces = aer_alloc((list->length ? list->length : 1) * sizeof(value_t));
    uint32_t total = 0;
    for (uint32_t i = 0; i < list->length; i++) {
        pieces[i] = aer_to_string(list->data.items[i]);
        total += pieces[i].as.obj->length + (i > 0 ? sep_length : 0);
    }
    value_t result = aer_string("", 0);
    result.as.obj->data.chars = aer_realloc(result.as.obj->data.chars, total + 1);
    char* out = result.as.obj->data.chars;
    for (uint32_t i = 0; i < list->length; i++) {
        if (i > 0) {
            memcpy(out, sep, sep_length);
            out += sep_length;
        }
        memcpy(out, pieces[i].as.obj->data.chars, pieces[i].as.obj->length);
        out += pieces[i].as.obj->length;
        aer_release(pieces[i]);
    }
    *out = '\0';
    result.as.obj->length = total;
    aer_free(pieces);
    return result;
}

// lists and bytes

static value_t bi_list (int argc, value_t* argv, int line) {
    int32_t count = int_arg(argv, 0, line, "list");
    if (count < 0 || count > 1000000) {
        aer_error(line, "list() size must be 0 to 1000000");
    }
    value_t fill = argc > 1 ? argv[1] : aer_nil();
    value_t list = aer_list((uint32_t)count);
    for (int32_t i = 0; i < count; i++) {
        aer_retain(fill);
        aer_list_push(list.as.obj, fill);
    }
    return list;
}

static value_t bi_push (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* list = list_arg(argv, 0, line, "push");
    aer_retain(argv[1]);
    aer_list_push(list, argv[1]);
    return aer_nil();
}

static value_t bi_pop (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* list = list_arg(argv, 0, line, "pop");
    if (list->length == 0) {
        return aer_nil();
    }
    return list->data.items[--list->length]; // the list's reference passes to the caller
}

static value_t bi_insert (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* list = list_arg(argv, 0, line, "insert");
    int32_t at = int_arg(argv, 1, line, "insert");
    if (at < 0 || (uint32_t)at > list->length) {
        aer_error(line, "insert() position %d is out of range", at);
    }
    aer_retain(argv[2]);
    aer_list_push(list, aer_nil()); // make room
    memmove(&list->data.items[at + 1], &list->data.items[at], (list->length - 1 - (uint32_t)at) * sizeof(value_t));
    list->data.items[at] = argv[2];
    return aer_nil();
}

static value_t bi_remove (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* list = list_arg(argv, 0, line, "remove");
    int32_t at = int_arg(argv, 1, line, "remove");
    if (at < 0 || (uint32_t)at >= list->length) {
        aer_error(line, "remove() position %d is out of range", at);
    }
    value_t item = list->data.items[at];
    memmove(&list->data.items[at], &list->data.items[at + 1], (list->length - 1 - (uint32_t)at) * sizeof(value_t));
    list->length--;
    return item;
}

static value_t bi_bytes (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t length = int_arg(argv, 0, line, "bytes");
    if (length < 0 || length > 64 * 1024 * 1024) {
        aer_error(line, "bytes() size must be 0 to 64MB");
    }
    return aer_bytes((uint32_t)length);
}

// files

static value_t bi_read_file (int argc, value_t* argv, int line) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    (void)argc;
    if (!file_arg(argv, 0, line, "rdf", dir, name)) {
        return aer_nil();
    }
    char* text = fs_read_f(dir, name);
    if (text == 0) {
        return aer_nil();
    }
    value_t s = aer_string(text, (uint32_t)slen(text));
    free(text);
    return s;
}

static value_t bi_read_bytes (int argc, value_t* argv, int line) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    uint32_t size = 0;
    (void)argc;
    if (!file_arg(argv, 0, line, "rdb", dir, name)) {
        return aer_nil();
    }
    char* data = fs_read_bytes(dir, name, &size);
    if (data == 0) {
        return aer_nil();
    }
    value_t b = aer_bytes(size);
    memcpy(b.as.obj->data.bytes, data, size);
    free(data);
    return b;
}

static value_t save_file (value_t* argv, int line, const char* fn, bool append) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    if (!file_arg(argv, 0, line, fn, dir, name)) {
        return aer_bool(false);
    }
    value_t text = aer_to_string(argv[1]);
    int result;
    if (fs_file_exists(dir, name)) {
        result = fs_write_f(dir, name, text.as.obj->data.chars, append);
    } else {
        result = fs_create_f(dir, name, text.as.obj->data.chars);
    }
    aer_release(text);
    return aer_bool(result >= 0);
}

static value_t bi_write_file (int argc, value_t* argv, int line) {
    (void)argc;
    return save_file(argv, line, "wrf", false);
}

static value_t bi_append_file (int argc, value_t* argv, int line) {
    (void)argc;
    return save_file(argv, line, "apf", true);
}

static value_t bi_file_exists (int argc, value_t* argv, int line) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    (void)argc;
    if (!file_arg(argv, 0, line, "isf", dir, name)) {
        return aer_bool(false);
    }
    return aer_bool(fs_file_exists(dir, name));
}

static value_t bi_delete_file (int argc, value_t* argv, int line) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    (void)argc;
    if (!file_arg(argv, 0, line, "delf", dir, name)) {
        return aer_bool(false);
    }
    return aer_bool(fs_delete_f(dir, name) == FS_OK);
}

// add one directory entry to the list, directories get a '/' on the end
static void list_one (const char* name, bool is_dir, uint32_t size, void* ctx) {
    (void)size;
    char entry[FS_NAME_MAX + 2];
    scpy(entry, name, sizeof(entry));
    if (is_dir) {
        scat(entry, "/", sizeof(entry));
    }
    aer_list_push((aer_object_t*)ctx, aer_string(entry, (uint32_t)slen(entry)));
}

// the current directory, e.g. "/bin/"
static value_t bi_cwd (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    return aer_string(cd, (uint32_t)slen(cd));
}

static value_t bi_list_files (int argc, value_t* argv, int line) {
    const char* dir = argc > 0 ? str_arg(argv, 0, line, "la")->data.chars : ".";
    value_t list = aer_list(8);
    if (fs_list(dir, list_one, list.as.obj) < 0) {
        aer_release(list);
        return aer_nil();
    }
    return list;
}

// encrypted files (see sys/fcrypt.h)

static value_t bi_encrypt_file (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = str_arg(argv, 0, line, "enc")->data.chars;
    const char* password = str_arg(argv, 1, line, "enc")->data.chars;
    return aer_bool(fcrypt_encrypt_file(path, password) == FCRYPT_OK);
}

static value_t bi_decrypt_file (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = str_arg(argv, 0, line, "dec")->data.chars;
    const char* password = str_arg(argv, 1, line, "dec")->data.chars;
    return aer_bool(fcrypt_decrypt_file(path, password) == FCRYPT_OK);
}

static value_t bi_is_encrypted (int argc, value_t* argv, int line) {
    (void)argc;
    return aer_bool(fcrypt_file_is_encrypted(str_arg(argv, 0, line, "isenc")->data.chars));
}

// the contents of an encrypted file as a string, without changing the file (nil if it can't be read)
static value_t bi_read_encrypted (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = str_arg(argv, 0, line, "rdenc")->data.chars;
    const char* password = str_arg(argv, 1, line, "rdenc")->data.chars;
    uint8_t* contents;
    uint32_t length;
    if (fcrypt_read_file(path, password, &contents, &length) != FCRYPT_OK) {
        return aer_nil();
    }
    value_t s = aer_string((const char*)contents, length);
    wipe(contents, length);
    free(contents);
    return s;
}

// system

static value_t bi_sleep (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t ms = int_arg(argv, 0, line, "sleep");
    // sleep in small steps so ctrl + C still gets noticed
    while (ms > 0 && !kb_take_break()) {
        sleep_ms(ms > 20 ? 20 : (uint32_t)ms);
        ms -= 20;
    }
    if (ms > 0) {
        aer_stop(line);
    }
    return aer_nil();
}

// play() stops on Ctrl+C, and then the program stops too (like sleep)
static bool play_interrupted;

static bool play_should_stop (void) {
    if (kb_take_break()) {
        play_interrupted = true;
    }
    return play_interrupted;
}

// play a WAV or MP3 file and wait for it to finish, false if it couldn't be played
static value_t bi_play (int argc, value_t* argv, int line) {
    (void)argc;
    play_interrupted = false;
    int result = sound_play_file(str_arg(argv, 0, line, "play")->data.chars, play_should_stop);
    if (play_interrupted) {
        aer_stop(line);
    }
    return aer_bool(result == SOUND_OK);
}

// animation (the same drawing the OS uses, see sys/ui.h)

// a screen position from two arguments, checked so nothing is drawn off the screen
static void cell_args (value_t* argv, int line, const char* fn, uint8_t* x, uint8_t* y) {
    int32_t cx = int_arg(argv, 0, line, fn);
    int32_t cy = int_arg(argv, 1, line, fn);
    if (cx < 0 || cx >= COLS || cy < 0 || cy >= ROWS) {
        aer_error(line, "%s() position %d, %d is off the screen (it is %d x %d)", fn, cx, cy, COLS, ROWS);
    }
    *x = (uint8_t)cx;
    *y = (uint8_t)cy;
}

// big(x, y, text, colour, background): text in big letters, like the splash screen's
// "aerOS", with its top left corner at column x, row y. Each letter is 8 columns wide and 7 rows
// tall. The colour is WHITE and the background BLACK if they are left out.
// Returns how many columns wide it is, so it can be centred
static value_t bi_big_text (int argc, value_t* argv, int line) {
    uint8_t x, y;
    cell_args(argv, line, "big", &x, &y);
    aer_object_t* text = str_arg(argv, 2, line, "big");

    enum color colour = WHITE;
    if (argc > 3) {
        colour = (enum color)(int_arg(argv, 3, line, "big") & 15); // one of the 16 colours
    }
    enum color background = BLACK;
    if (argc > 4) {
        background = (enum color)(int_arg(argv, 4, line, "big") & 15);
    }
    return aer_int(big_text(x, y, colour, background, text->data.chars));
}

// progress(x, y, width, done, total, colour): [#####.....]  50%
static value_t bi_progress (int argc, value_t* argv, int line) {
    uint8_t x, y;
    cell_args(argv, line, "progress", &x, &y);
    int32_t width = int_arg(argv, 2, line, "progress");
    int32_t done = int_arg(argv, 3, line, "progress");
    int32_t total = int_arg(argv, 4, line, "progress");
    int32_t colour = argc > 5 ? int_arg(argv, 5, line, "progress") : LIGHT_GREEN;
    if (width < 1 || width > COLS) {
        aer_error(line, "progress() width must be 1 to %d", COLS);
    }
    // a negative amount counts as none
    if (done < 0) {
        done = 0;
    }
    if (total < 0) {
        total = 0;
    }
    enum color bar_colour = (enum color)(colour & 15); // one of the 16 text colours
    ui_bar(x, y, (uint8_t)width, (uint32_t)done, (uint32_t)total, bar_colour);
    return aer_nil();
}

// spinner(x, y, style, colour): the frame for this moment, returns how many cells it took
static value_t bi_spinner (int argc, value_t* argv, int line) {
    uint8_t x, y;
    cell_args(argv, line, "spinner", &x, &y);
    int32_t style = argc > 2 ? int_arg(argv, 2, line, "spinner") : 0;
    int32_t colour = argc > 3 ? int_arg(argv, 3, line, "spinner") : YELLOW;
    if (style < 0 || style >= UI_SPINNER_STYLES) {
        aer_error(line, "spinner() style must be 0 to %d", UI_SPINNER_STYLES - 1);
    }
    return aer_int(ui_spinner(x, y, style, (enum color)(colour & 15)));
}

// frame(fps): wait until it's time for the next frame, so a loop runs at a steady speed
// whatever it draws (sleep would add the drawing time on top). Ctrl+C stops the program
static uint32_t last_frame_ms = 0;

static value_t bi_frame (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t fps = int_arg(argv, 0, line, "frame");
    if (fps < 1 || fps > 1000) {
        aer_error(line, "frame() needs 1 to 1000 frames a second");
    }
    uint32_t period = 1000u / (uint32_t)fps;
    // a long gap (the first frame, or after something slow) starts the timing again
    if (clkuptms() - last_frame_ms > period * 4) {
        last_frame_ms = clkuptms();
    }
    while (clkuptms() - last_frame_ms < period) {
        if (kb_take_break()) {
            aer_stop(line);
        }
        sleep_ms(1);
    }
    last_frame_ms += period;
    return aer_nil();
}

// queue("hello.aer a b"): put a program (and its arguments) at the end of the queue the GO
// command runs, or "s:text" to print a line between programs. Returns how many are now queued,
// or -1 if the process stack is full. The line isn't checked until it runs
static value_t bi_queue (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* code = str_arg(argv, 0, line, "queue");
    if (code->length == 0) {
        aer_error(line, "queue() needs a program to queue");
    }
    return aer_int(p_queue_add(code->data.chars));
}

// add one row of system information to the list as [group, name, value]
static void info_row (const char* group, const char* name, const char* value, void* ctx) {
    value_t row = aer_list(3);
    aer_list_push(row.as.obj, aer_string(group, (uint32_t)slen(group)));
    aer_list_push(row.as.obj, aer_string(name, (uint32_t)slen(name)));
    aer_list_push(row.as.obj, aer_string(value, (uint32_t)slen(value)));
    aer_list_push((aer_object_t*)ctx, row);
}

// everything AerOS knows about the computer, as a list of [group, name, value] (see sys/sysinfo.h)
static value_t bi_system_info (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    value_t rows = aer_list(64);
    sysinfo_collect(info_row, rows.as.obj);
    return rows;
}

static value_t bi_time (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    return aer_int((int32_t)clkuptms());
}

static value_t bi_random (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t range = int_arg(argv, 0, line, "random");
    if (range <= 0) {
        aer_error(line, "random() needs a number above 0");
    }
    return aer_int(rand() % range);
}

static value_t bi_beep (int argc, value_t* argv, int line) {
    beep(argc > 0 ? int_arg(argv, 0, line, "beep") : 0);
    return aer_nil();
}

// graphics

static void need_graphics (int line, const char* fn) {
    if (!aer->graphics) {
        aer_error(line, "%s() needs graphics, call gopen() first", fn);
    }
}

static value_t bi_gfx_open (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    if (!aer->graphics) {
        aer->graphics = gfx_begin();
    }
    return aer_bool(aer->graphics);
}

static value_t bi_gfx_close (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    if (aer->graphics) {
        gfx_end();
        aer->graphics = false;
    }
    return aer_nil();
}

static value_t bi_gfx_width (int argc, value_t* argv, int line) {
    (void)argc; (void)argv;
    need_graphics(line, "gw");
    return aer_int((int32_t)gfx_width());
}

static value_t bi_gfx_height (int argc, value_t* argv, int line) {
    (void)argc; (void)argv;
    need_graphics(line, "gh");
    return aer_int((int32_t)gfx_height());
}

static value_t bi_rgb (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t r = int_arg(argv, 0, line, "rgb") & 0xFF;
    int32_t g = int_arg(argv, 1, line, "rgb") & 0xFF;
    int32_t b = int_arg(argv, 2, line, "rgb") & 0xFF;
    return aer_int((r << 16) | (g << 8) | b);
}

static value_t bi_gfx_clear (int argc, value_t* argv, int line) {
    need_graphics(line, "gclear");
    gfx_clear(argc > 0 ? (uint32_t)int_arg(argv, 0, line, "gclear") : 0);
    return aer_nil();
}

static value_t bi_pixel (int argc, value_t* argv, int line) {
    (void)argc;
    need_graphics(line, "pixel");
    gfx_pixel(int_arg(argv, 0, line, "pixel"), int_arg(argv, 1, line, "pixel"), (uint32_t)int_arg(argv, 2, line, "pixel"));
    return aer_nil();
}

static value_t bi_rect (int argc, value_t* argv, int line) {
    (void)argc;
    need_graphics(line, "rect");
    gfx_rect(int_arg(argv, 0, line, "rect"), int_arg(argv, 1, line, "rect"), int_arg(argv, 2, line, "rect"),
             int_arg(argv, 3, line, "rect"), (uint32_t)int_arg(argv, 4, line, "rect"));
    return aer_nil();
}

static value_t bi_line (int argc, value_t* argv, int line) {
    (void)argc;
    need_graphics(line, "line");
    gfx_line(int_arg(argv, 0, line, "line"), int_arg(argv, 1, line, "line"), int_arg(argv, 2, line, "line"),
             int_arg(argv, 3, line, "line"), (uint32_t)int_arg(argv, 4, line, "line"));
    return aer_nil();
}

static value_t bi_text (int argc, value_t* argv, int line) {
    need_graphics(line, "text");
    value_t s = aer_to_string(argv[2]);
    int scale = argc > 4 ? int_arg(argv, 4, line, "text") : 1;
    gfx_text(int_arg(argv, 0, line, "text"), int_arg(argv, 1, line, "text"), s.as.obj->data.chars,
             (uint32_t)int_arg(argv, 3, line, "text"), scale);
    aer_release(s);
    return aer_nil();
}

static aer_object_t* bytes_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type != V_BYTES) {
        aer_error(line, "%s() needs bytes (from rdb) for argument %d, not a %s", fn, i + 1, aer_type_name(argv[i]));
    }
    return argv[i].as.obj;
}

static value_t bi_image (int argc, value_t* argv, int line) {
    need_graphics(line, "image");
    aer_object_t* data = bytes_arg(argv, 0, line, "image");
    int max_w = argc > 3 ? int_arg(argv, 3, line, "image") : 0;
    int max_h = argc > 4 ? int_arg(argv, 4, line, "image") : 0;
    return aer_bool(gfx_image(data->data.bytes, data->length, int_arg(argv, 1, line, "image"),
                              int_arg(argv, 2, line, "image"), max_w, max_h));
}

static value_t bi_image_size (int argc, value_t* argv, int line) {
    (void)argc;
    aer_object_t* data = bytes_arg(argv, 0, line, "imgsz");
    int w, h;
    if (!gfx_image_size(data->data.bytes, data->length, &w, &h)) {
        return aer_nil();
    }
    value_t size = aer_list(2);
    aer_list_push(size.as.obj, aer_int(w));
    aer_list_push(size.as.obj, aer_int(h));
    return size;
}

static value_t bi_exit (int argc, value_t* argv, int line) {
    (void)argc; (void)argv;
    // leaving by the error route, but quietly: there is nothing wrong
    (void)line;
    longjmp(aer->error, 2);
}

const aer_builtin_t aer_builtins[] = {
    { "print", bi_print, 0, AER_MAX_ARGS },
    { "write", bi_write, 0, AER_MAX_ARGS },
    { "input", bi_input, 0, 1 },
    { "inpw", bi_input_secret, 0, 1 },
    { "cls", bi_cls, 0, 0 },
    { "color", bi_color, 1, 2 },
    { "at", bi_at, 2, 2 },
    { "key", bi_key, 0, 0 },
    { "haskey", bi_haskey, 0, 0 },
    { "len", bi_len, 1, 1 },
    { "str", bi_str, 1, 1 },
    { "int", bi_int, 1, 1 },
    { "chr", bi_chr, 1, 1 },
    { "ord", bi_ord, 1, 1 },
    { "sub", bi_sub, 2, 3 },
    { "find", bi_find, 2, 2 },
    { "upper", bi_upper, 1, 1 },
    { "lower", bi_lower, 1, 1 },
    { "split", bi_split, 1, 2 },
    { "join", bi_join, 1, 2 },
    { "list", bi_list, 1, 2 },
    { "push", bi_push, 2, 2 },
    { "pop", bi_pop, 1, 1 },
    { "insert", bi_insert, 3, 3 },
    { "remove", bi_remove, 2, 2 },
    { "bytes", bi_bytes, 1, 1 },
    { "rdf", bi_read_file, 1, 1 },
    { "rdb", bi_read_bytes, 1, 1 },
    { "wrf", bi_write_file, 2, 2 },
    { "apf", bi_append_file, 2, 2 },
    { "isf", bi_file_exists, 1, 1 },
    { "delf", bi_delete_file, 1, 1 },
    { "enc", bi_encrypt_file, 2, 2 },
    { "dec", bi_decrypt_file, 2, 2 },
    { "isenc", bi_is_encrypted, 1, 1 },
    { "rdenc", bi_read_encrypted, 2, 2 },
    { "la", bi_list_files, 0, 1 },
    { "cwd", bi_cwd, 0, 0 },
    { "sleep", bi_sleep, 1, 1 },
    { "time", bi_time, 0, 0 },
    { "sysinfo", bi_system_info, 0, 0 },
    { "big", bi_big_text, 3, 5 },
    { "progress", bi_progress, 5, 6 },
    { "spinner", bi_spinner, 2, 4 },
    { "frame", bi_frame, 1, 1 },
    { "queue", bi_queue, 1, 1 },
    { "random", bi_random, 1, 1 },
    { "beep", bi_beep, 0, 1 },
    { "play", bi_play, 1, 1 },
    { "exit", bi_exit, 0, 0 },
    { "gopen", bi_gfx_open, 0, 0 },
    { "gclose", bi_gfx_close, 0, 0 },
    { "gw", bi_gfx_width, 0, 0 },
    { "gh", bi_gfx_height, 0, 0 },
    { "gclear", bi_gfx_clear, 0, 1 },
    { "rgb", bi_rgb, 3, 3 },
    { "pixel", bi_pixel, 3, 3 },
    { "rect", bi_rect, 5, 5 },
    { "line", bi_line, 5, 5 },
    { "text", bi_text, 4, 5 },
    { "image", bi_image, 3, 5 },
    { "imgsz", bi_image_size, 1, 1 },
};

const int aer_builtin_count = sizeof(aer_builtins) / sizeof(aer_builtins[0]);

void aer_define_constants (void) {
    static const struct { const char* name; int32_t value; } constants[] = {
        { "KEY_UP", KEY_UP }, { "KEY_DOWN", KEY_DOWN }, { "KEY_LEFT", KEY_LEFT },
        { "KEY_RIGHT", KEY_RIGHT }, { "KEY_HOME", KEY_HOME }, { "KEY_END", KEY_END },
        { "KEY_DELETE", KEY_DELETE }, { "KEY_ESCAPE", KEY_ESCAPE }, { "KEY_ENTER", '\n' },
        { "KEY_BACKSPACE", '\b' }, { "KEY_TAB", '\t' }, { "KEY_PGUP", KEY_PGUP }, { "KEY_PGDN", KEY_PGDN },
        { "BLACK", BLACK }, { "DARK_BLUE", DARK_BLUE }, { "GREEN", GREEN }, { "CYAN", CYAN },
        { "RED", RED }, { "PURPLE", PURPLE }, { "ORANGE", ORANGE }, { "BROWN", BROWN }, { "BRIGHT", BRIGHT },
        { "DARK_GRAY", DARK_GRAY }, { "LIGHT_BLUE", LIGHT_BLUE }, { "LIGHT_GREEN", LIGHT_GREEN },
        { "LIGHT_CYAN", LIGHT_CYAN }, { "LIGHT_RED", LIGHT_RED }, { "LIGHT_PURPLE", LIGHT_PURPLE },
        { "YELLOW", YELLOW }, { "WHITE", WHITE },
    };
    for (size_t i = 0; i < sizeof(constants) / sizeof(constants[0]); i++) {
        aer_define_global(constants[i].name, aer_int(constants[i].value));
    }
    srand(clkuptms()); // a different random() sequence every run
}

// a program that ends (or crashes) in graphics mode gets the terminal back automatically
void aer_builtins_finish (void) {
    if (aer->graphics) {
        gfx_end();
        aer->graphics = false;
    }
}

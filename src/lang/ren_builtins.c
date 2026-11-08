/*
    ren_builtins.c
    The functions every RenLang program can call: printing and input, strings, lists, byte
    buffers, files and a few system services. Each one gets its arguments as an array of values
    (which it must not release) and hands back a new value.
*/

#include "ren_internal.h"
#include "../lib/ren_string.h"
#include "../lib/ren_stdlib.h"
#include "../lib/ren_stdio.h"
#include "../lib/ren_ctype.h"
#include "../drivers/video/VGA_linear.h"
#include "../drivers/video/gfx.h"
#include "../drivers/input/IO.h"
#include "../drivers/audio/tone.h"
#include "../sys/sys_clck.h"
#include "../sys/f_sys.h"
#include "../sys/fcrypt.h"
#include "../lib/crypto.h"

// argument checks

static int32_t int_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type != V_INT) {
        ren_error(line, "%s() needs a number for argument %d, not a %s", fn, i + 1, ren_type_name(argv[i]));
    }
    return argv[i].as.i;
}

static ren_object_t* str_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type != V_STR) {
        ren_error(line, "%s() needs a string for argument %d, not a %s", fn, i + 1, ren_type_name(argv[i]));
    }
    return argv[i].as.obj;
}

static ren_object_t* list_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type != V_LIST) {
        ren_error(line, "%s() needs a list for argument %d, not a %s", fn, i + 1, ren_type_name(argv[i]));
    }
    return argv[i].as.obj;
}

// split a path typed by a program into directory and file name
static bool file_arg (value_t* argv, int i, int line, const char* fn, char* dir, char* name) {
    ren_object_t* path = str_arg(argv, i, line, fn);
    return fs_split_path(path->data.chars, dir, FS_PATH_MAX, name, FS_NAME_MAX);
}

// console

static void print_values (int argc, value_t* argv) {
    for (int i = 0; i < argc; i++) {
        if (i > 0) console_putchar_raw(' ');
        ren_print_value(argv[i]);
    }
}

static value_t bi_print (int argc, value_t* argv, int line) {
    (void)line;
    print_values(argc, argv);
    console_putchar('\n');
    return ren_nil();
}

static value_t bi_write (int argc, value_t* argv, int line) {
    (void)line;
    print_values(argc, argv);
    console_update_cursor();
    return ren_nil();
}

static value_t bi_input (int argc, value_t* argv, int line) {
    char buffer[256];
    if (argc > 0) ren_print_value(argv[0]);
    (void)line;
    GetString(buffer, sizeof(buffer));
    return ren_string(buffer, (uint32_t)strlen(buffer));
}

// like input(), but shows * for each character (for passwords)
static value_t bi_input_secret (int argc, value_t* argv, int line) {
    char buffer[256];
    if (argc > 0) ren_print_value(argv[0]);
    (void)line;
    GetSecret(buffer, sizeof(buffer));
    value_t s = ren_string(buffer, (uint32_t)strlen(buffer));
    crypto_wipe(buffer, sizeof(buffer));
    return s;
}

static value_t bi_cls (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    console_clear();
    return ren_nil();
}

static value_t bi_color (int argc, value_t* argv, int line) {
    int32_t fg = int_arg(argv, 0, line, "color");
    int32_t bg = argc > 1 ? int_arg(argv, 1, line, "color") : (int32_t)console_bg;
    console_set_color((enum color)(fg & 15), (enum color)(bg & 15));
    return ren_nil();
}

static value_t bi_at (int argc, value_t* argv, int line) {
    (void)argc;
    console_set_cursor((uint8_t)int_arg(argv, 0, line, "at"), (uint8_t)int_arg(argv, 1, line, "at"));
    return ren_nil();
}

static value_t bi_key (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    return ren_int(GetChar());
}

static value_t bi_haskey (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    return ren_bool(kb_haschar());
}

// strings and conversions

static value_t bi_len (int argc, value_t* argv, int line) {
    (void)argc;
    if (argv[0].type != V_STR && argv[0].type != V_LIST && argv[0].type != V_BYTES) {
        ren_error(line, "len() needs a string, list or bytes, not a %s", ren_type_name(argv[0]));
    }
    return ren_int((int32_t)argv[0].as.obj->length);
}

static value_t bi_str (int argc, value_t* argv, int line) {
    (void)argc; (void)line;
    return ren_to_string(argv[0]);
}

static value_t bi_int (int argc, value_t* argv, int line) {
    (void)argc; (void)line;
    switch (argv[0].type) {
        case V_INT: return argv[0];
        case V_BOOL: return ren_int(argv[0].as.i);
        case V_STR: {
            char* end;
            const char* text = argv[0].as.obj->data.chars;
            long value = strtol(text, &end, 0);
            while (isspace((unsigned char)*end)) end++;
            if (end == text || *end != '\0') return ren_nil(); // not a number
            return ren_int((int32_t)value);
        }
        default: return ren_nil();
    }
}

static value_t bi_chr (int argc, value_t* argv, int line) {
    (void)argc;
    char c = (char)int_arg(argv, 0, line, "chr");
    return ren_string(&c, 1);
}

static value_t bi_ord (int argc, value_t* argv, int line) {
    (void)argc;
    ren_object_t* s = str_arg(argv, 0, line, "ord");
    if (s->length == 0) ren_error(line, "ord() needs a string with at least one character");
    return ren_int((uint8_t)s->data.chars[0]);
}

static value_t bi_sub (int argc, value_t* argv, int line) {
    ren_object_t* s = str_arg(argv, 0, line, "sub");
    int32_t start = int_arg(argv, 1, line, "sub");
    int32_t count = argc > 2 ? int_arg(argv, 2, line, "sub") : (int32_t)s->length;
    if (start < 0) start = 0;
    if ((uint32_t)start > s->length) start = (int32_t)s->length;
    if (count < 0) count = 0;
    if ((uint32_t)count > s->length - (uint32_t)start) count = (int32_t)(s->length - (uint32_t)start);
    return ren_string(s->data.chars + start, (uint32_t)count);
}

static value_t bi_find (int argc, value_t* argv, int line) {
    (void)argc;
    ren_object_t* s = str_arg(argv, 0, line, "find");
    ren_object_t* needle = str_arg(argv, 1, line, "find");
    char* found = strstr(s->data.chars, needle->data.chars);
    return ren_int(found ? (int32_t)(found - s->data.chars) : -1);
}

static value_t change_case (value_t* argv, int line, const char* fn, bool upper) {
    ren_object_t* s = str_arg(argv, 0, line, fn);
    value_t result = ren_string(s->data.chars, s->length);
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
    ren_object_t* s = str_arg(argv, 0, line, "split");
    const char* sep = argc > 1 ? str_arg(argv, 1, line, "split")->data.chars : 0;
    size_t sep_length = sep ? strlen(sep) : 0;
    if (sep && sep_length == 0) ren_error(line, "split() needs a separator that isn't empty");

    value_t list = ren_list(4);
    const char* p = s->data.chars;
    const char* end = p + s->length;
    if (!sep) {
        while (p < end) {
            while (p < end && *p == ' ') p++;
            const char* start = p;
            while (p < end && *p != ' ') p++;
            if (p > start) ren_list_push(list.as.obj, ren_string(start, (uint32_t)(p - start)));
        }
        return list;
    }
    for (;;) {
        const char* hit = strstr(p, sep);
        if (hit == 0) {
            ren_list_push(list.as.obj, ren_string(p, (uint32_t)(end - p)));
            return list;
        }
        ren_list_push(list.as.obj, ren_string(p, (uint32_t)(hit - p)));
        p = hit + sep_length;
    }
}

static value_t bi_join (int argc, value_t* argv, int line) {
    ren_object_t* list = list_arg(argv, 0, line, "join");
    const char* sep = argc > 1 ? str_arg(argv, 1, line, "join")->data.chars : "";
    uint32_t sep_length = (uint32_t)strlen(sep);

    // turn every item into a string first, so the total length is known
    value_t* pieces = ren_alloc((list->length ? list->length : 1) * sizeof(value_t));
    uint32_t total = 0;
    for (uint32_t i = 0; i < list->length; i++) {
        pieces[i] = ren_to_string(list->data.items[i]);
        total += pieces[i].as.obj->length + (i > 0 ? sep_length : 0);
    }
    value_t result = ren_string("", 0);
    result.as.obj->data.chars = ren_realloc(result.as.obj->data.chars, total + 1);
    char* out = result.as.obj->data.chars;
    for (uint32_t i = 0; i < list->length; i++) {
        if (i > 0) {
            memcpy(out, sep, sep_length);
            out += sep_length;
        }
        memcpy(out, pieces[i].as.obj->data.chars, pieces[i].as.obj->length);
        out += pieces[i].as.obj->length;
        ren_release(pieces[i]);
    }
    *out = '\0';
    result.as.obj->length = total;
    ren_free(pieces);
    return result;
}

// lists and bytes

static value_t bi_list (int argc, value_t* argv, int line) {
    int32_t count = int_arg(argv, 0, line, "list");
    if (count < 0 || count > 1000000) ren_error(line, "list() size must be 0 to 1000000");
    value_t fill = argc > 1 ? argv[1] : ren_nil();
    value_t list = ren_list((uint32_t)count);
    for (int32_t i = 0; i < count; i++) {
        ren_retain(fill);
        ren_list_push(list.as.obj, fill);
    }
    return list;
}

static value_t bi_push (int argc, value_t* argv, int line) {
    (void)argc;
    ren_object_t* list = list_arg(argv, 0, line, "push");
    ren_retain(argv[1]);
    ren_list_push(list, argv[1]);
    return ren_nil();
}

static value_t bi_pop (int argc, value_t* argv, int line) {
    (void)argc;
    ren_object_t* list = list_arg(argv, 0, line, "pop");
    if (list->length == 0) return ren_nil();
    return list->data.items[--list->length]; // the list's reference passes to the caller
}

static value_t bi_insert (int argc, value_t* argv, int line) {
    (void)argc;
    ren_object_t* list = list_arg(argv, 0, line, "insert");
    int32_t at = int_arg(argv, 1, line, "insert");
    if (at < 0 || (uint32_t)at > list->length) ren_error(line, "insert() position %d is out of range", at);
    ren_retain(argv[2]);
    ren_list_push(list, ren_nil()); // make room
    memmove(&list->data.items[at + 1], &list->data.items[at], (list->length - 1 - (uint32_t)at) * sizeof(value_t));
    list->data.items[at] = argv[2];
    return ren_nil();
}

static value_t bi_remove (int argc, value_t* argv, int line) {
    (void)argc;
    ren_object_t* list = list_arg(argv, 0, line, "remove");
    int32_t at = int_arg(argv, 1, line, "remove");
    if (at < 0 || (uint32_t)at >= list->length) ren_error(line, "remove() position %d is out of range", at);
    value_t item = list->data.items[at];
    memmove(&list->data.items[at], &list->data.items[at + 1], (list->length - 1 - (uint32_t)at) * sizeof(value_t));
    list->length--;
    return item;
}

static value_t bi_bytes (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t length = int_arg(argv, 0, line, "bytes");
    if (length < 0 || length > 64 * 1024 * 1024) ren_error(line, "bytes() size must be 0 to 64MB");
    return ren_bytes((uint32_t)length);
}

// files

static value_t bi_read_file (int argc, value_t* argv, int line) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    (void)argc;
    if (!file_arg(argv, 0, line, "read_file", dir, name)) return ren_nil();
    char* text = fs_read_f(dir, name);
    if (text == 0) return ren_nil();
    value_t s = ren_string(text, (uint32_t)strlen(text));
    free(text);
    return s;
}

static value_t bi_read_bytes (int argc, value_t* argv, int line) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    uint32_t size = 0;
    (void)argc;
    if (!file_arg(argv, 0, line, "read_bytes", dir, name)) return ren_nil();
    char* data = fs_read_bytes(dir, name, &size);
    if (data == 0) return ren_nil();
    value_t b = ren_bytes(size);
    memcpy(b.as.obj->data.bytes, data, size);
    free(data);
    return b;
}

static value_t save_file (value_t* argv, int line, const char* fn, bool append) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    if (!file_arg(argv, 0, line, fn, dir, name)) return ren_bool(false);
    value_t text = ren_to_string(argv[1]);
    int result;
    if (fs_file_exists(dir, name)) {
        result = fs_write_f(dir, name, text.as.obj->data.chars, append);
    } else {
        result = fs_create_f(dir, name, text.as.obj->data.chars);
    }
    ren_release(text);
    return ren_bool(result >= 0);
}

static value_t bi_write_file (int argc, value_t* argv, int line) {
    (void)argc;
    return save_file(argv, line, "write_file", false);
}

static value_t bi_append_file (int argc, value_t* argv, int line) {
    (void)argc;
    return save_file(argv, line, "append_file", true);
}

static value_t bi_file_exists (int argc, value_t* argv, int line) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    (void)argc;
    if (!file_arg(argv, 0, line, "file_exists", dir, name)) return ren_bool(false);
    return ren_bool(fs_file_exists(dir, name));
}

// encrypted files (see sys/fcrypt.h)

static value_t bi_encrypt_file (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = str_arg(argv, 0, line, "encrypt_file")->data.chars;
    const char* password = str_arg(argv, 1, line, "encrypt_file")->data.chars;
    return ren_bool(fcrypt_encrypt_file(path, password) == FCRYPT_OK);
}

static value_t bi_decrypt_file (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = str_arg(argv, 0, line, "decrypt_file")->data.chars;
    const char* password = str_arg(argv, 1, line, "decrypt_file")->data.chars;
    return ren_bool(fcrypt_decrypt_file(path, password) == FCRYPT_OK);
}

static value_t bi_is_encrypted (int argc, value_t* argv, int line) {
    (void)argc;
    return ren_bool(fcrypt_file_is_encrypted(str_arg(argv, 0, line, "is_encrypted")->data.chars));
}

// the contents of an encrypted file as a string, without changing the file (nil if it can't be read)
static value_t bi_read_encrypted (int argc, value_t* argv, int line) {
    (void)argc;
    const char* path = str_arg(argv, 0, line, "read_encrypted")->data.chars;
    const char* password = str_arg(argv, 1, line, "read_encrypted")->data.chars;
    uint8_t* contents;
    uint32_t length;
    if (fcrypt_read_file(path, password, &contents, &length) != FCRYPT_OK) return ren_nil();
    value_t s = ren_string((const char*)contents, length);
    crypto_wipe(contents, length);
    free(contents);
    return s;
}

static value_t bi_delete_file (int argc, value_t* argv, int line) {
    char dir[FS_PATH_MAX], name[FS_NAME_MAX];
    (void)argc;
    if (!file_arg(argv, 0, line, "delete_file", dir, name)) return ren_bool(false);
    return ren_bool(fs_delete_f(dir, name) == FS_OK);
}

// add one directory entry to the list, directories get a '/' on the end
static void list_one (const char* name, bool is_dir, uint32_t size, void* ctx) {
    (void)size;
    char entry[FS_NAME_MAX + 2];
    strlcpy(entry, name, sizeof(entry));
    if (is_dir) strlcat(entry, "/", sizeof(entry));
    ren_list_push((ren_object_t*)ctx, ren_string(entry, (uint32_t)strlen(entry)));
}

// the current directory, e.g. "/bin/"
static value_t bi_cwd (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    return ren_string(cd, (uint32_t)strlen(cd));
}

static value_t bi_list_files (int argc, value_t* argv, int line) {
    const char* dir = argc > 0 ? str_arg(argv, 0, line, "list_files")->data.chars : ".";
    value_t list = ren_list(8);
    if (fs_list(dir, list_one, list.as.obj) < 0) {
        ren_release(list);
        return ren_nil();
    }
    return list;
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
    if (ms > 0) ren_error(line, "stopped (Ctrl+C)");
    return ren_nil();
}

static value_t bi_time (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    return ren_int((int32_t)clck_uptime_ms());
}

static value_t bi_random (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t range = int_arg(argv, 0, line, "random");
    if (range <= 0) ren_error(line, "random() needs a number above 0");
    return ren_int(rand() % range);
}

static value_t bi_beep (int argc, value_t* argv, int line) {
    beep(argc > 0 ? int_arg(argv, 0, line, "beep") : 0);
    return ren_nil();
}

// graphics

static void need_graphics (int line, const char* fn) {
    if (!ren->graphics) ren_error(line, "%s() needs graphics, call gfx_open() first", fn);
}

static value_t bi_gfx_open (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    if (!ren->graphics) {
        ren->graphics = gfx_begin();
    }
    return ren_bool(ren->graphics);
}

static value_t bi_gfx_close (int argc, value_t* argv, int line) {
    (void)argc; (void)argv; (void)line;
    if (ren->graphics) {
        gfx_end();
        ren->graphics = false;
    }
    return ren_nil();
}

static value_t bi_gfx_width (int argc, value_t* argv, int line) {
    (void)argc; (void)argv;
    need_graphics(line, "gfx_width");
    return ren_int((int32_t)gfx_width());
}

static value_t bi_gfx_height (int argc, value_t* argv, int line) {
    (void)argc; (void)argv;
    need_graphics(line, "gfx_height");
    return ren_int((int32_t)gfx_height());
}

static value_t bi_rgb (int argc, value_t* argv, int line) {
    (void)argc;
    int32_t r = int_arg(argv, 0, line, "rgb") & 0xFF;
    int32_t g = int_arg(argv, 1, line, "rgb") & 0xFF;
    int32_t b = int_arg(argv, 2, line, "rgb") & 0xFF;
    return ren_int((r << 16) | (g << 8) | b);
}

static value_t bi_gfx_clear (int argc, value_t* argv, int line) {
    need_graphics(line, "gfx_clear");
    gfx_clear(argc > 0 ? (uint32_t)int_arg(argv, 0, line, "gfx_clear") : 0);
    return ren_nil();
}

static value_t bi_pixel (int argc, value_t* argv, int line) {
    (void)argc;
    need_graphics(line, "pixel");
    gfx_pixel(int_arg(argv, 0, line, "pixel"), int_arg(argv, 1, line, "pixel"), (uint32_t)int_arg(argv, 2, line, "pixel"));
    return ren_nil();
}

static value_t bi_rect (int argc, value_t* argv, int line) {
    (void)argc;
    need_graphics(line, "rect");
    gfx_rect(int_arg(argv, 0, line, "rect"), int_arg(argv, 1, line, "rect"), int_arg(argv, 2, line, "rect"),
             int_arg(argv, 3, line, "rect"), (uint32_t)int_arg(argv, 4, line, "rect"));
    return ren_nil();
}

static value_t bi_line (int argc, value_t* argv, int line) {
    (void)argc;
    need_graphics(line, "line");
    gfx_line(int_arg(argv, 0, line, "line"), int_arg(argv, 1, line, "line"), int_arg(argv, 2, line, "line"),
             int_arg(argv, 3, line, "line"), (uint32_t)int_arg(argv, 4, line, "line"));
    return ren_nil();
}

static value_t bi_text (int argc, value_t* argv, int line) {
    need_graphics(line, "text");
    value_t s = ren_to_string(argv[2]);
    int scale = argc > 4 ? int_arg(argv, 4, line, "text") : 1;
    gfx_text(int_arg(argv, 0, line, "text"), int_arg(argv, 1, line, "text"), s.as.obj->data.chars,
             (uint32_t)int_arg(argv, 3, line, "text"), scale);
    ren_release(s);
    return ren_nil();
}

static ren_object_t* bytes_arg (value_t* argv, int i, int line, const char* fn) {
    if (argv[i].type != V_BYTES) {
        ren_error(line, "%s() needs bytes (from read_bytes) for argument %d, not a %s", fn, i + 1, ren_type_name(argv[i]));
    }
    return argv[i].as.obj;
}

static value_t bi_image (int argc, value_t* argv, int line) {
    need_graphics(line, "image");
    ren_object_t* data = bytes_arg(argv, 0, line, "image");
    int max_w = argc > 3 ? int_arg(argv, 3, line, "image") : 0;
    int max_h = argc > 4 ? int_arg(argv, 4, line, "image") : 0;
    return ren_bool(gfx_image(data->data.bytes, data->length, int_arg(argv, 1, line, "image"),
                              int_arg(argv, 2, line, "image"), max_w, max_h));
}

static value_t bi_image_size (int argc, value_t* argv, int line) {
    (void)argc;
    ren_object_t* data = bytes_arg(argv, 0, line, "image_size");
    int w, h;
    if (!gfx_image_size(data->data.bytes, data->length, &w, &h)) return ren_nil();
    value_t size = ren_list(2);
    ren_list_push(size.as.obj, ren_int(w));
    ren_list_push(size.as.obj, ren_int(h));
    return size;
}

static value_t bi_exit (int argc, value_t* argv, int line) {
    (void)argc; (void)argv;
    // leaving by the error route, but quietly: there is nothing wrong
    (void)line;
    longjmp(ren->error, 2);
}

const ren_builtin_t ren_builtins[] = {
    { "print", bi_print, 0, REN_MAX_ARGS },
    { "write", bi_write, 0, REN_MAX_ARGS },
    { "input", bi_input, 0, 1 },
    { "input_secret", bi_input_secret, 0, 1 },
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
    { "read_file", bi_read_file, 1, 1 },
    { "read_bytes", bi_read_bytes, 1, 1 },
    { "write_file", bi_write_file, 2, 2 },
    { "append_file", bi_append_file, 2, 2 },
    { "file_exists", bi_file_exists, 1, 1 },
    { "delete_file", bi_delete_file, 1, 1 },
    { "encrypt_file", bi_encrypt_file, 2, 2 },
    { "decrypt_file", bi_decrypt_file, 2, 2 },
    { "is_encrypted", bi_is_encrypted, 1, 1 },
    { "read_encrypted", bi_read_encrypted, 2, 2 },
    { "list_files", bi_list_files, 0, 1 },
    { "cwd", bi_cwd, 0, 0 },
    { "sleep", bi_sleep, 1, 1 },
    { "time", bi_time, 0, 0 },
    { "random", bi_random, 1, 1 },
    { "beep", bi_beep, 0, 1 },
    { "exit", bi_exit, 0, 0 },
    { "gfx_open", bi_gfx_open, 0, 0 },
    { "gfx_close", bi_gfx_close, 0, 0 },
    { "gfx_width", bi_gfx_width, 0, 0 },
    { "gfx_height", bi_gfx_height, 0, 0 },
    { "gfx_clear", bi_gfx_clear, 0, 1 },
    { "rgb", bi_rgb, 3, 3 },
    { "pixel", bi_pixel, 3, 3 },
    { "rect", bi_rect, 5, 5 },
    { "line", bi_line, 5, 5 },
    { "text", bi_text, 4, 5 },
    { "image", bi_image, 3, 5 },
    { "image_size", bi_image_size, 1, 1 },
};

const int ren_builtin_count = sizeof(ren_builtins) / sizeof(ren_builtins[0]);

void ren_define_constants (void) {
    static const struct { const char* name; int32_t value; } constants[] = {
        { "KEY_UP", KEY_UP }, { "KEY_DOWN", KEY_DOWN }, { "KEY_LEFT", KEY_LEFT },
        { "KEY_RIGHT", KEY_RIGHT }, { "KEY_HOME", KEY_HOME }, { "KEY_END", KEY_END },
        { "KEY_DELETE", KEY_DELETE }, { "KEY_ESCAPE", KEY_ESCAPE }, { "KEY_ENTER", '\n' },
        { "KEY_BACKSPACE", '\b' },
        { "BLACK", BLACK }, { "DARK_BLUE", DARK_BLUE }, { "GREEN", GREEN }, { "CYAN", CYAN },
        { "RED", RED }, { "PURPLE", PURPLE }, { "BROWN", BROWN }, { "BRIGHT", BRIGHT },
        { "DARK_GRAY", DARK_GRAY }, { "LIGHT_BLUE", LIGHT_BLUE }, { "LIGHT_GREEN", LIGHT_GREEN },
        { "LIGHT_CYAN", LIGHT_CYAN }, { "LIGHT_RED", LIGHT_RED }, { "LIGHT_PURPLE", LIGHT_PURPLE },
        { "YELLOW", YELLOW }, { "WHITE", WHITE },
    };
    for (size_t i = 0; i < sizeof(constants) / sizeof(constants[0]); i++) {
        ren_define_global(constants[i].name, ren_int(constants[i].value));
    }
    srand(clck_uptime_ms()); // a different random() sequence every run
}

// a program that ends (or crashes) in graphics mode gets the terminal back automatically
void ren_builtins_finish (void) {
    if (ren->graphics) {
        gfx_end();
        ren->graphics = false;
    }
}
